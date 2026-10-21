/*
 * x87 FPU (presented to the game as a 387), registers held as doubles.
 *
 * The game's Borland runtime writes floating point either as real x87
 * instructions or as its emulator interrupts (INT 34h..3Dh); the decoder
 * turns both into OP_FPU. Borland's INT 3Eh "shortcuts" (transcendental
 * helpers of its math library) land in fpu_shortcut().
 *
 * A real 387 keeps 80-bit intermediates; doubles differ only in the last
 * bits. Exceptions are always handled as if masked.
 */
#include "ops.h"

#include <math.h>
#include <stdio.h>

typedef struct {
	double st[8];
	uint8_t tag[8]; /* 0 valid, 1 zero, 2 special, 3 empty */
	int top;
	uint16_t cw;
	uint16_t sw; /* without TOP */
} Fpu;

static Fpu fpu = { .cw = 0x037F, .tag = { 3, 3, 3, 3, 3, 3, 3, 3 } };

#define SW_IE 0x0001
#define SW_ZE 0x0004
#define SW_C0 0x0100
#define SW_C1 0x0200
#define SW_C2 0x0400
#define SW_C3 0x4000
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

static inline double *st(int i)
{
	return &fpu.st[(fpu.top + i) & 7];
}

static uint8_t tag_of(double v)
{
	if (v == 0)
		return 1;
	if (!isfinite(v) || fpclassify(v) == FP_SUBNORMAL)
		return 2;
	return 0;
}

static void push(double v)
{
	fpu.top = (fpu.top - 1) & 7;
	if (fpu.tag[fpu.top] != 3)
		fpu.sw |= SW_IE | 0x40 | SW_C1; /* stack overflow */
	fpu.st[fpu.top] = v;
	fpu.tag[fpu.top] = tag_of(v);
}

static void pop(void)
{
	fpu.tag[fpu.top] = 3;
	fpu.top = (fpu.top + 1) & 7;
}

static void set_st(int i, double v)
{
	int p = (fpu.top + i) & 7;
	fpu.st[p] = v;
	fpu.tag[p] = tag_of(v);
}

uint16_t fpu_status(void)
{
	return (uint16_t)((fpu.sw & ~0x3800) | ((fpu.top & 7) << 11));
}

static uint16_t tag_word(void)
{
	uint16_t t = 0;
	int i;
	for (i = 0; i < 8; i++)
		t |= (uint16_t)((fpu.tag[i] & 3) << (2 * i));
	return t;
}

void fpu_reset(void)
{
	int i;
	fpu.cw = 0x037F;
	fpu.sw = 0;
	fpu.top = 0;
	for (i = 0; i < 8; i++)
		fpu.tag[i] = 3;
}

/* ---- memory formats ---------------------------------------------------------- */

static double ext_to_double(uint32_t a)
{
	uint64_t mant = rd32(a) | ((uint64_t)rd32(a + 4) << 32);
	uint16_t se = rd16(a + 8);
	int exp = se & 0x7FFF;
	double r;

	if (exp == 0 && mant == 0)
		r = 0.0;
	else if (exp == 0x7FFF)
		r = (mant << 1) == 0 ? INFINITY : NAN;
	else
		r = ldexp((double)mant, exp - 16383 - 63);
	return (se & 0x8000) ? -r : r;
}

static void double_to_ext(double v, uint32_t a)
{
	uint64_t mant;
	int exp, sign = signbit(v) != 0;

	if (v == 0) {
		mant = 0;
		exp = 0;
	} else if (isinf(v)) {
		mant = 0x8000000000000000ull;
		exp = 0x7FFF;
	} else if (isnan(v)) {
		mant = 0xC000000000000000ull;
		exp = 0x7FFF;
	} else {
		int e;
		double m = frexp(fabs(v), &e);
		mant = (uint64_t)ldexp(m, 64);
		exp = e - 1 + 16383;
	}
	wr32(a, (uint32_t)mant);
	wr32(a + 4, (uint32_t)(mant >> 32));
	wr16(a + 8, (uint16_t)(exp | (sign ? 0x8000 : 0)));
}

static double rd_f32(uint32_t a)
{
	uint32_t b = rd32(a);
	float f;
	memcpy(&f, &b, 4);
	return f;
}

static double rd_f64(uint32_t a)
{
	uint64_t b = rd32(a) | ((uint64_t)rd32(a + 4) << 32);
	double d;
	memcpy(&d, &b, 8);
	return d;
}

static void wr_f32(uint32_t a, double v)
{
	float f = (float)v;
	uint32_t b;
	memcpy(&b, &f, 4);
	wr32(a, b);
}

static void wr_f64(uint32_t a, double v)
{
	uint64_t b;
	memcpy(&b, &v, 8);
	wr32(a, (uint32_t)b);
	wr32(a + 4, (uint32_t)(b >> 32));
}

static int64_t rd_i64(uint32_t a)
{
	return (int64_t)(rd32(a) | ((uint64_t)rd32(a + 4) << 32));
}

static double round_cw(double v)
{
	switch ((fpu.cw >> 10) & 3) {
	case 0: return nearbyint(v);
	case 1: return floor(v);
	case 2: return ceil(v);
	default: return trunc(v);
	}
}

/* integer store with the "integer indefinite" value on overflow */
static void store_int(uint32_t a, double v, int bytes)
{
	double r = round_cw(v);

	if (bytes == 2) {
		if (!(r >= -32768.0 && r <= 32767.0)) {
			fpu.sw |= SW_IE;
			wr16(a, 0x8000);
		} else {
			wr16(a, (uint16_t)(int16_t)r);
		}
	} else if (bytes == 4) {
		if (!(r >= -2147483648.0 && r <= 2147483647.0)) {
			fpu.sw |= SW_IE;
			wr32(a, 0x80000000u);
		} else {
			wr32(a, (uint32_t)(int32_t)r);
		}
	} else {
		uint64_t b;
		if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
			b = 0x8000000000000000ull;
		else
			b = (uint64_t)(int64_t)r;
		wr32(a, (uint32_t)b);
		wr32(a + 4, (uint32_t)(b >> 32));
	}
}

static double rd_bcd(uint32_t a)
{
	double v = 0;
	int i;
	for (i = 8; i >= 0; i--) {
		uint8_t b = rd8(a + i);
		v = v * 100 + (b >> 4) * 10 + (b & 15);
	}
	return (rd8(a + 9) & 0x80) ? -v : v;
}

static void wr_bcd(uint32_t a, double v)
{
	double r = round_cw(v);
	int neg = r < 0, i;
	uint64_t n;

	if (!(fabs(r) < 1e18)) {
		/* BCD indefinite */
		for (i = 0; i < 7; i++)
			wr8(a + i, 0);
		wr8(a + 7, 0xC0);
		wr8(a + 8, 0xFF);
		wr8(a + 9, 0xFF);
		return;
	}
	n = (uint64_t)fabs(r);
	for (i = 0; i < 9; i++) {
		uint8_t lo = (uint8_t)(n % 10);
		n /= 10;
		wr8(a + i, (uint8_t)(lo | ((n % 10) << 4)));
		n /= 10;
	}
	wr8(a + 9, neg ? 0x80 : 0);
}

/* ---- arithmetic ---------------------------------------------------------------- */

static void compare(double a, double b)
{
	fpu.sw &= ~SW_CC;
	if (isnan(a) || isnan(b))
		fpu.sw |= SW_C0 | SW_C2 | SW_C3;
	else if (a < b)
		fpu.sw |= SW_C0;
	else if (a == b)
		fpu.sw |= SW_C3;
}

/* the eight arithmetic ops by modrm reg field, d = destination, s = source */
static double arith(int op, double d, double s)
{
	switch (op) {
	case 0: return d + s;
	case 1: return d * s;
	case 4: return d - s;
	case 5: return s - d;
	case 6:
		if (s == 0 && d == d)
			fpu.sw |= SW_ZE;
		return d / s;
	default:
		if (d == 0 && s == s)
			fpu.sw |= SW_ZE;
		return s / d;
	}
}

/* ST(0) op memory operand */
static void arith_mem(int op, double m)
{
	if (op == 2 || op == 3) {
		compare(*st(0), m);
		if (op == 3)
			pop();
		return;
	}
	set_st(0, arith(op, *st(0), m));
}

static void fxam(void)
{
	double v = *st(0);
	fpu.sw &= ~SW_CC;
	if (signbit(v))
		fpu.sw |= SW_C1;
	if (fpu.tag[fpu.top] == 3)
		fpu.sw |= SW_C3 | SW_C0;
	else if (isnan(v))
		fpu.sw |= SW_C0;
	else if (isinf(v))
		fpu.sw |= SW_C2 | SW_C0;
	else if (v == 0)
		fpu.sw |= SW_C3;
	else if (fpclassify(v) == FP_SUBNORMAL)
		fpu.sw |= SW_C3 | SW_C2;
	else
		fpu.sw |= SW_C2;
}

static void fprem(int ieee)
{
	double a = *st(0), b = *st(1), r, q;
	uint64_t qi;

	if (b == 0 || isinf(a) || isnan(a) || isnan(b)) {
		fpu.sw |= SW_IE;
		set_st(0, NAN);
		return;
	}
	r = ieee ? remainder(a, b) : fmod(a, b);
	q = ieee ? nearbyint(a / b) : trunc(a / b);
	qi = fabs(q) < 1.8e19 ? (uint64_t)fabs(q) : 0;
	fpu.sw &= ~SW_CC;
	if (qi & 1)
		fpu.sw |= SW_C1;
	if (qi & 2)
		fpu.sw |= SW_C3;
	if (qi & 4)
		fpu.sw |= SW_C0;
	set_st(0, r);
}

static void store_env(uint32_t a)
{
	wr16(a, fpu.cw);
	wr16(a + 2, fpu_status());
	wr16(a + 4, tag_word());
	wr16(a + 6, 0);
	wr16(a + 8, 0);
	wr16(a + 10, 0);
	wr16(a + 12, 0);
}

static void load_env(uint32_t a)
{
	uint16_t sw = rd16(a + 2), tw = rd16(a + 4);
	int i;
	fpu.cw = rd16(a);
	fpu.sw = sw & ~0x3800;
	fpu.top = (sw >> 11) & 7;
	for (i = 0; i < 8; i++)
		fpu.tag[i] = (tw >> (2 * i)) & 3;
}

static int reg_form(int esc, int reg, int rm)
{
	double t;

	switch (esc) {
	case 0: /* D8: ST(0) = ST(0) op ST(i) */
		if (reg == 2 || reg == 3) {
			compare(*st(0), *st(rm));
			if (reg == 3)
				pop();
		} else {
			set_st(0, arith(reg, *st(0), *st(rm)));
		}
		return 0;
	case 1: /* D9 */
		switch (reg) {
		case 0: t = *st(rm); push(t); return 0;           /* FLD ST(i) */
		case 1: {                                         /* FXCH */
			int p0 = fpu.top, pi = (fpu.top + rm) & 7;
			uint8_t tg = fpu.tag[p0];
			t = fpu.st[p0];
			fpu.st[p0] = fpu.st[pi];
			fpu.st[pi] = t;
			fpu.tag[p0] = fpu.tag[pi];
			fpu.tag[pi] = tg;
			return 0;
		}
		case 2: return rm == 0 ? 0 : 1;                   /* FNOP */
		case 3: set_st(rm, *st(0)); pop(); return 0;      /* FSTP1 (undocumented) */
		case 4:
			switch (rm) {
			case 0: set_st(0, -*st(0)); return 0;
			case 1: set_st(0, fabs(*st(0))); return 0;
			case 4: compare(*st(0), 0.0); return 0;
			case 5: fxam(); return 0;
			}
			return 1;
		case 5: {
			static const double k[7] = { 1.0, 3.32192809488736234787, 1.44269504088896340736,
			                             3.14159265358979323846, 0.30102999566398119521,
			                             0.69314718055994530942, 0.0 };
			if (rm == 7)
				return 1;
			push(k[rm]);
			return 0;
		}
		case 6:
			switch (rm) {
			case 0: set_st(0, expm1(*st(0) * M_LN2)); return 0;                     /* F2XM1 */
			case 1: set_st(1, *st(1) * log2(*st(0))); pop(); return 0;              /* FYL2X */
			case 2: set_st(0, tan(*st(0))); fpu.sw &= ~SW_C2; push(1.0); return 0;  /* FPTAN */
			case 3: set_st(1, atan2(*st(1), *st(0))); pop(); return 0;              /* FPATAN */
			case 4: {                                                               /* FXTRACT */
				double v = *st(0);
				if (v == 0) {
					fpu.sw |= SW_ZE;
					set_st(0, -INFINITY);
					push(v);
				} else {
					int e = ilogb(v);
					set_st(0, (double)e);
					push(scalbn(v, -e));
				}
				return 0;
			}
			case 5: fprem(1); return 0;
			case 6: fpu.top = (fpu.top - 1) & 7; return 0;
			case 7: fpu.top = (fpu.top + 1) & 7; return 0;
			}
			return 1;
		default: /* 7 */
			switch (rm) {
			case 0: fprem(0); return 0;
			case 1: set_st(1, *st(1) * log1p(*st(0)) / M_LN2); pop(); return 0;    /* FYL2XP1 */
			case 2: set_st(0, sqrt(*st(0))); return 0;
			case 3: t = *st(0); set_st(0, sin(t)); push(cos(t)); fpu.sw &= ~SW_C2; return 0;
			case 4: set_st(0, round_cw(*st(0))); return 0;
			case 5: {
				double sc = trunc(*st(1));
				int e = sc > 100000 ? 100000 : sc < -100000 ? -100000 : (int)sc;
				set_st(0, scalbn(*st(0), e));
				return 0;
			}
			case 6: set_st(0, sin(*st(0))); fpu.sw &= ~SW_C2; return 0;
			case 7: set_st(0, cos(*st(0))); fpu.sw &= ~SW_C2; return 0;
			}
			return 1;
		}
	case 2: /* DA E9: FUCOMPP */
		if (reg == 5 && rm == 1) {
			compare(*st(0), *st(1));
			pop();
			pop();
			return 0;
		}
		return 1;
	case 3: /* DB */
		if (reg == 4) {
			switch (rm) {
			case 0: case 1: case 4: return 0; /* FNENI FNDISI FNSETPM: no-ops on a 387 */
			case 2: fpu.sw &= 0x7F00; return 0; /* FNCLEX */
			case 3: fpu_reset(); return 0;     /* FNINIT */
			}
		}
		return 1;
	case 4: /* DC: ST(i) = ST(i) op ST(0), with SUB/SUBR and DIV/DIVR swapped */
		if (reg == 2 || reg == 3) {
			compare(*st(0), *st(rm));
			if (reg == 3)
				pop();
			return 0;
		}
		set_st(rm, arith(reg ^ (reg >= 4), *st(rm), *st(0)));
		return 0;
	case 5: /* DD */
		switch (reg) {
		case 0: fpu.tag[(fpu.top + rm) & 7] = 3; return 0; /* FFREE */
		case 2: set_st(rm, *st(0)); return 0;               /* FST ST(i) */
		case 3: set_st(rm, *st(0)); pop(); return 0;        /* FSTP ST(i) */
		case 4: compare(*st(0), *st(rm)); return 0;          /* FUCOM */
		case 5: compare(*st(0), *st(rm)); pop(); return 0;   /* FUCOMP */
		}
		return 1;
	case 6: /* DE: arithmetic and pop */
		if (reg == 3) {
			if (rm != 1)
				return 1;
			compare(*st(0), *st(1)); /* FCOMPP */
			pop();
			pop();
			return 0;
		}
		if (reg == 2)
			return 1;
		set_st(rm, arith(reg ^ (reg >= 4), *st(rm), *st(0)));
		pop();
		return 0;
	default: /* DF */
		if (reg == 4 && rm == 0) {
			rAX = fpu_status(); /* FNSTSW AX */
			return 0;
		}
		if (reg == 0) { /* FFREEP */
			fpu.tag[(fpu.top + rm) & 7] = 3;
			pop();
			return 0;
		}
		return 1;
	}
}

int fpu_exec(const Insn *in)
{
	int esc = in->fpu_esc, reg = (in->modrm >> 3) & 7, rm = in->modrm & 7;
	uint32_t a;

	if (in->modrm >= 0xC0)
		return reg_form(esc, reg, rm);
	a = ea_linear(&in->a);
	switch (esc) {
	case 0: arith_mem(reg, rd_f32(a)); return 0;
	case 2: arith_mem(reg, (int32_t)rd32(a)); return 0;
	case 4: arith_mem(reg, rd_f64(a)); return 0;
	case 6: arith_mem(reg, (int16_t)rd16(a)); return 0;
	case 1:
		switch (reg) {
		case 0: push(rd_f32(a)); return 0;
		case 2: wr_f32(a, *st(0)); return 0;
		case 3: wr_f32(a, *st(0)); pop(); return 0;
		case 4: load_env(a); return 0;
		case 5: fpu.cw = rd16(a); return 0;
		case 6: store_env(a); return 0;
		case 7: wr16(a, fpu.cw); return 0;
		}
		return 1;
	case 3:
		switch (reg) {
		case 0: push((int32_t)rd32(a)); return 0;
		case 2: store_int(a, *st(0), 4); return 0;
		case 3: store_int(a, *st(0), 4); pop(); return 0;
		case 5: push(ext_to_double(a)); return 0;
		case 7: double_to_ext(*st(0), a); pop(); return 0;
		}
		return 1;
	case 5:
		switch (reg) {
		case 0: push(rd_f64(a)); return 0;
		case 2: wr_f64(a, *st(0)); return 0;
		case 3: wr_f64(a, *st(0)); pop(); return 0;
		case 4: { /* FRSTOR */
			int i;
			load_env(a);
			for (i = 0; i < 8; i++)
				fpu.st[(fpu.top + i) & 7] = ext_to_double(a + 14 + 10 * i);
			return 0;
		}
		case 6: { /* FNSAVE */
			int i;
			store_env(a);
			for (i = 0; i < 8; i++)
				double_to_ext(fpu.st[(fpu.top + i) & 7], a + 14 + 10 * i);
			fpu_reset();
			return 0;
		}
		case 7: wr16(a, fpu_status()); return 0;
		}
		return 1;
	default: /* 7: DF */
		switch (reg) {
		case 0: push((int16_t)rd16(a)); return 0;
		case 2: store_int(a, *st(0), 2); return 0;
		case 3: store_int(a, *st(0), 2); pop(); return 0;
		case 4: push(rd_bcd(a)); return 0;
		case 5: push((double)rd_i64(a)); return 0;
		case 6: wr_bcd(a, *st(0)); pop(); return 0;
		case 7: store_int(a, *st(0), 8); pop(); return 0;
		}
		return 1;
	}
}

/* Borland emulator INT 3Eh helpers of the math library, on ST(0) */
void fpu_shortcut(int code)
{
	double v = *st(0);
	switch (code) {
	case 0xEC: v = sin(v); break;
	case 0xEE: v = cos(v); break;
	case 0xF0: v = tan(v); break;
	case 0xF2: v = atan(v); break;
	case 0xF4: v = log(v); break;
	case 0xFA: v = exp(v); break;
	default:
		fprintf(stderr, "fpu: unknown Borland INT 3Eh shortcut %02x\n", code);
		return;
	}
	set_st(0, v);
}

/* for debugging dumps */
double fpu_st(int i)
{
	return *st(i);
}
