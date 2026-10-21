/*
 * Semantics of the straight-line (non-branching) instructions.
 *
 * Each exec_* function takes a decoded instruction. The interpreter calls
 * them with instructions decoded at run time; the recompiled game code
 * calls them with `static const Insn` constants, so after inlining GCC folds
 * the operand decoding away and only the operation itself remains. Both
 * paths therefore share one definition of every instruction.
 *
 * Functions that can fault return nonzero: the caller then raises the
 * exception (the interpreter) or hands the instruction to the interpreter
 * (compiled code).
 */
#ifndef OPS_H
#define OPS_H

#include "cpu.h"
#include "insn.h"

uint32_t port_in(uint16_t port, int size);
void port_out(uint16_t port, uint32_t v, int size);

/* out of line: strings, BCD, FPU (ops.c / fpu.c) */
void str_movs(int size, int seg, int rep);
void str_stos(int size, int rep);
void str_lods(int size, int seg, int rep);
void str_cmps(int size, int seg, int rep);
void str_scas(int size, int rep);
void str_ins(int size, int rep);
void str_outs(int size, int seg, int rep);
void bcd_daa(void);
void bcd_das(void);
void bcd_aaa(void);
void bcd_aas(void);
int fpu_exec(const Insn *in);
void fpu_shortcut(int code);

/* ---- operands ----------------------------------------------------------------- */

ALWAYS_INLINE uint16_t ea_offset(const Operand *o)
{
	uint16_t base;
	switch (o->ea) {
	case EA_BX_SI: base = (uint16_t)(rBX + rSI); break;
	case EA_BX_DI: base = (uint16_t)(rBX + rDI); break;
	case EA_BP_SI: base = (uint16_t)(rBP + rSI); break;
	case EA_BP_DI: base = (uint16_t)(rBP + rDI); break;
	case EA_SI: base = rSI; break;
	case EA_DI: base = rDI; break;
	case EA_BP: base = rBP; break;
	case EA_BX: base = rBX; break;
	default: base = 0; break;
	}
	return (uint16_t)(base + o->disp);
}

ALWAYS_INLINE uint32_t ea_linear(const Operand *o)
{
	return LIN(cpu.s[o->seg], ea_offset(o));
}

ALWAYS_INLINE uint32_t opnd_read(const Operand *o, int size)
{
	switch (o->type) {
	case OT_REG: return reg_get(o->reg, size);
	case OT_IMM: return o->imm;
	case OT_SREG: return cpu.s[o->reg];
	default: return rdn(ea_linear(o), size);
	}
}

ALWAYS_INLINE void opnd_write(const Operand *o, uint32_t v, int size)
{
	switch (o->type) {
	case OT_REG: reg_set(o->reg, v, size); break;
	case OT_SREG: cpu.s[o->reg] = (uint16_t)v; break;
	default: wrn(ea_linear(o), v, size); break;
	}
}

/* read-modify-write on operand a: the address is computed once */
#define RMW_READ(o, sz, lin) ((o)->type == OT_MEM ? rdn((lin) = ea_linear(o), sz) : reg_get((o)->reg, sz))
#define RMW_WRITE(o, sz, lin, v)                                                                  \
	do {                                                                                      \
		if ((o)->type == OT_MEM)                                                          \
			wrn(lin, v, sz);                                                          \
		else                                                                              \
			reg_set((o)->reg, v, sz);                                                 \
	} while (0)

/* ---- flag helpers ----------------------------------------------------------------- */

ALWAYS_INLINE uint32_t sz_mask(int sz)
{
	return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

ALWAYS_INLINE uint32_t sz_sign(int sz)
{
	return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u;
}

/* `m` (FM_*) says which flags to compute: the recompiler leaves out dead ones */
ALWAYS_INLINE void set_szp(uint32_t r, int sz, int m)
{
	if (m & FM_ZF)
		cpu.zf = r == 0;
	if (m & FM_SF)
		cpu.sf = (r & sz_sign(sz)) != 0;
	if (m & FM_PF)
		cpu.pfv = (uint8_t)r;
}

ALWAYS_INLINE uint32_t alu_add(uint32_t a, uint32_t b, uint32_t c, int sz, int m)
{
	uint32_t r;
	if (sz == 4) {
		uint64_t t = (uint64_t)a + b + c;
		r = (uint32_t)t;
		if (m & FM_CF)
			cpu.cf = (uint8_t)(t >> 32);
	} else {
		uint32_t t = a + b + c;
		r = t & sz_mask(sz);
		if (m & FM_CF)
			cpu.cf = (t >> (sz * 8)) & 1;
	}
	if (m & FM_OF)
		cpu.of = (((a ^ r) & (b ^ r)) & sz_sign(sz)) != 0;
	if (m & FM_AF)
		cpu.af = (a ^ b ^ r) & 0x10;
	set_szp(r, sz, m);
	return r;
}

ALWAYS_INLINE uint32_t alu_sub(uint32_t a, uint32_t b, uint32_t c, int sz, int m)
{
	uint32_t r;
	if (sz == 4) {
		uint64_t t = (uint64_t)a - b - c;
		r = (uint32_t)t;
		if (m & FM_CF)
			cpu.cf = (uint8_t)((t >> 32) & 1);
	} else {
		uint32_t t = a - b - c;
		r = t & sz_mask(sz);
		if (m & FM_CF)
			cpu.cf = (t >> (sz * 8)) & 1;
	}
	if (m & FM_OF)
		cpu.of = (((a ^ b) & (a ^ r)) & sz_sign(sz)) != 0;
	if (m & FM_AF)
		cpu.af = (a ^ b ^ r) & 0x10;
	set_szp(r, sz, m);
	return r;
}

ALWAYS_INLINE uint32_t alu_logic(uint32_t r, int sz, int m)
{
	if (m & FM_CF)
		cpu.cf = 0;
	if (m & FM_OF)
		cpu.of = 0;
	if (m & FM_AF)
		cpu.af = 0;
	set_szp(r, sz, m);
	return r;
}

ALWAYS_INLINE int cond(int cc)
{
	switch (cc) {
	case 0: return cpu.of;
	case 1: return !cpu.of;
	case 2: return cpu.cf;
	case 3: return !cpu.cf;
	case 4: return cpu.zf;
	case 5: return !cpu.zf;
	case 6: return cpu.cf | cpu.zf;
	case 7: return !(cpu.cf | cpu.zf);
	case 8: return cpu.sf;
	case 9: return !cpu.sf;
	case 10: return get_pf();
	case 11: return !get_pf();
	case 12: return cpu.sf != cpu.of;
	case 13: return cpu.sf == cpu.of;
	case 14: return cpu.zf | (cpu.sf != cpu.of);
	default: return !cpu.zf & (cpu.sf == cpu.of);
	}
}

/* ---- ALU ------------------------------------------------------------------------- */

/* ADD OR ADC SBB AND SUB XOR CMP TEST: a op= b */
ALWAYS_INLINE void exec_alu(const Insn *in)
{
	const int sz = in->osz, m = in->fmask;
	uint32_t lin = 0, a, b, r;

	if (in->op == OP_CMP || in->op == OP_TEST)
		a = opnd_read(&in->a, sz);
	else
		a = RMW_READ(&in->a, sz, lin);
	b = opnd_read(&in->b, sz);
	switch (in->op) {
	case OP_ADD: r = alu_add(a, b, 0, sz, m); break;
	case OP_ADC: r = alu_add(a, b, cpu.cf, sz, m); break;
	case OP_SUB: r = alu_sub(a, b, 0, sz, m); break;
	case OP_SBB: r = alu_sub(a, b, cpu.cf, sz, m); break;
	case OP_CMP: alu_sub(a, b, 0, sz, m); return;
	case OP_AND: r = alu_logic(a & b, sz, m); break;
	case OP_OR: r = alu_logic(a | b, sz, m); break;
	case OP_XOR: r = alu_logic(a ^ b, sz, m); break;
	default: /* TEST */ alu_logic(a & b, sz, m); return;
	}
	RMW_WRITE(&in->a, sz, lin, r);
}

/* ---- scalar primitives: also called directly by the recompiled code ---- */

ALWAYS_INLINE uint32_t alu_inc(uint32_t a, int sz, int m)
{
	uint32_t r = (a + 1) & sz_mask(sz);
	if (m & FM_OF)
		cpu.of = r == sz_sign(sz);
	if (m & FM_AF)
		cpu.af = (a ^ 1 ^ r) & 0x10;
	set_szp(r, sz, m);
	return r;
}

ALWAYS_INLINE uint32_t alu_dec(uint32_t a, int sz, int m)
{
	uint32_t r = (a - 1) & sz_mask(sz);
	if (m & FM_OF)
		cpu.of = a == sz_sign(sz);
	if (m & FM_AF)
		cpu.af = (a ^ 1 ^ r) & 0x10;
	set_szp(r, sz, m);
	return r;
}

/* MUL / IMUL with one operand: AL*s -> AX, AX*s -> DX:AX */
ALWAYS_INLINE void alu_mul1(int is_signed, uint32_t s, int sz, int m)
{
	if (!is_signed) {
		if (sz == 1) {
			rAX = (uint16_t)(rAL * s);
			if (m & (FM_CF | FM_OF))
				cpu.cf = cpu.of = rAH != 0;
		} else if (sz == 2) {
			uint32_t r = (uint32_t)rAX * s;
			rAX = (uint16_t)r;
			rDX = (uint16_t)(r >> 16);
			if (m & (FM_CF | FM_OF))
				cpu.cf = cpu.of = rDX != 0;
		} else {
			uint64_t r = (uint64_t)cpu.r[R_AX].d * s;
			cpu.r[R_AX].d = (uint32_t)r;
			cpu.r[R_DX].d = (uint32_t)(r >> 32);
			if (m & (FM_CF | FM_OF))
				cpu.cf = cpu.of = cpu.r[R_DX].d != 0;
		}
	} else {
		if (sz == 1) {
			int16_t r = (int16_t)((int8_t)rAL * (int8_t)s);
			rAX = (uint16_t)r;
			if (m & (FM_CF | FM_OF))
				cpu.cf = cpu.of = r != (int8_t)r;
		} else if (sz == 2) {
			int32_t r = (int32_t)(int16_t)rAX * (int16_t)s;
			rAX = (uint16_t)r;
			rDX = (uint16_t)((uint32_t)r >> 16);
			if (m & (FM_CF | FM_OF))
				cpu.cf = cpu.of = r != (int16_t)r;
		} else {
			int64_t r = (int64_t)(int32_t)cpu.r[R_AX].d * (int32_t)s;
			cpu.r[R_AX].d = (uint32_t)r;
			cpu.r[R_DX].d = (uint32_t)((uint64_t)r >> 32);
			if (m & (FM_CF | FM_OF))
				cpu.cf = cpu.of = r != (int32_t)r;
		}
	}
}

/* two/three operand IMUL: returns b * c */
ALWAYS_INLINE uint32_t alu_imul(uint32_t b, uint32_t c, int sz, int m)
{
	if (sz == 2) {
		int32_t r = (int32_t)(int16_t)b * (int16_t)c;
		if (m & (FM_CF | FM_OF))
			cpu.cf = cpu.of = r != (int16_t)r;
		return (uint16_t)r;
	} else {
		int64_t r = (int64_t)(int32_t)b * (int32_t)c;
		if (m & (FM_CF | FM_OF))
			cpu.cf = cpu.of = r != (int32_t)r;
		return (uint32_t)r;
	}
}

/* DIV / IDIV by s: returns 1 on divide error (nothing changed then) */
ALWAYS_INLINE int alu_div(int is_signed, uint32_t s, int sz)
{
	if (s == 0)
		return 1;
	if (!is_signed) {
		if (sz == 1) {
			uint32_t q = rAX / s;
			if (q > 0xFF)
				return 1;
			rAH = (uint8_t)(rAX % s);
			rAL = (uint8_t)q;
		} else if (sz == 2) {
			uint32_t n = ((uint32_t)rDX << 16) | rAX, q = n / s;
			if (q > 0xFFFF)
				return 1;
			rDX = (uint16_t)(n % s);
			rAX = (uint16_t)q;
		} else {
			uint64_t n = ((uint64_t)cpu.r[R_DX].d << 32) | cpu.r[R_AX].d, q = n / s;
			if (q > 0xFFFFFFFFu)
				return 1;
			cpu.r[R_DX].d = (uint32_t)(n % s);
			cpu.r[R_AX].d = (uint32_t)q;
		}
	} else {
		if (sz == 1) {
			int32_t n = (int16_t)rAX, d = (int8_t)s, q;
			q = n / d;
			if (q > 127 || q < -128)
				return 1;
			rAH = (uint8_t)(n % d);
			rAL = (uint8_t)q;
		} else if (sz == 2) {
			int32_t n = (int32_t)(((uint32_t)rDX << 16) | rAX), d = (int16_t)s;
			int32_t q, r;
			if (n == INT32_MIN && d == -1)
				return 1;
			q = n / d;
			r = n % d;
			if (q > 32767 || q < -32768)
				return 1;
			rDX = (uint16_t)r;
			rAX = (uint16_t)q;
		} else {
			int64_t n = (int64_t)(((uint64_t)cpu.r[R_DX].d << 32) | cpu.r[R_AX].d), d = (int32_t)s, q;
			if (n == INT64_MIN && d == -1)
				return 1;
			q = n / d;
			if (q > INT32_MAX || q < INT32_MIN)
				return 1;
			cpu.r[R_DX].d = (uint32_t)(n % d);
			cpu.r[R_AX].d = (uint32_t)q;
		}
	}
	return 0;
}

/* ROL ROR RCL RCR SHL SHR SAL SAR of a by count (1..31, already masked) */
ALWAYS_INLINE uint32_t alu_shift(int op, uint32_t a, uint32_t count, int sz, int fm)
{
	const int bits = sz * 8;
	const uint32_t m = sz_mask(sz), sign = sz_sign(sz);
	uint32_t r;

	switch (op) {
	case OP_ROL: {
		uint32_t n = count % bits;
		r = n ? ((a << n) | (a >> (bits - n))) & m : a;
		if (fm & (FM_CF | FM_OF)) {
			cpu.cf = r & 1;
			cpu.of = ((r & sign) != 0) ^ cpu.cf;
		}
		break;
	}
	case OP_ROR: {
		uint32_t n = count % bits;
		r = n ? ((a >> n) | (a << (bits - n))) & m : a;
		if (fm & FM_CF)
			cpu.cf = (r & sign) != 0;
		if (fm & FM_OF)
			cpu.of = ((r ^ (r << 1)) & sign) != 0;
		break;
	}
	case OP_RCL: {
		uint32_t n = count % (bits + 1), cf = cpu.cf;
		r = a;
		while (n--) {
			uint32_t out = (r & sign) != 0;
			r = ((r << 1) | cf) & m;
			cf = out;
		}
		cpu.cf = (uint8_t)cf;
		cpu.of = ((r & sign) != 0) ^ cpu.cf;
		break;
	}
	case OP_RCR: {
		uint32_t n = count % (bits + 1), cf = cpu.cf;
		r = a;
		while (n--) {
			uint32_t out = r & 1;
			r = (r >> 1) | (cf ? sign : 0);
			cf = out;
		}
		cpu.cf = (uint8_t)cf;
		cpu.of = ((r ^ (r << 1)) & sign) != 0;
		break;
	}
	case OP_SHL: case OP_SAL: {
		uint8_t cf = count <= (uint32_t)bits ? (uint8_t)(((uint64_t)a >> (bits - count)) & 1) : 0;
		r = (uint32_t)(((uint64_t)a << count) & m);
		if (fm & FM_CF)
			cpu.cf = cf;
		if (fm & FM_OF)
			cpu.of = ((r & sign) != 0) ^ cf;
		if (fm & FM_AF)
			cpu.af = 0;
		set_szp(r, sz, fm);
		break;
	}
	case OP_SHR:
		if (fm & FM_CF)
			cpu.cf = (uint8_t)(((uint64_t)a >> (count - 1)) & 1);
		r = (uint32_t)((uint64_t)a >> count);
		if (fm & FM_OF)
			cpu.of = (a & sign) != 0;
		if (fm & FM_AF)
			cpu.af = 0;
		set_szp(r, sz, fm);
		break;
	default: { /* SAR */
		int64_t sa = sz == 1 ? (int8_t)a : sz == 2 ? (int16_t)a : (int32_t)a;
		if (fm & FM_CF)
			cpu.cf = (uint8_t)((sa >> (count - 1)) & 1);
		r = (uint32_t)(sa >> count) & m;
		if (fm & FM_OF)
			cpu.of = 0;
		if (fm & FM_AF)
			cpu.af = 0;
		set_szp(r, sz, fm);
		break;
	}
	}
	return r;
}

/* ---- the same, on decoded instructions (the interpreter) ---- */

ALWAYS_INLINE void exec_incdec(const Insn *in)
{
	const int sz = in->osz;
	uint32_t lin = 0, a = RMW_READ(&in->a, sz, lin);
	uint32_t r = in->op == OP_INC ? alu_inc(a, sz, in->fmask) : alu_dec(a, sz, in->fmask);
	RMW_WRITE(&in->a, sz, lin, r);
}

ALWAYS_INLINE void exec_neg(const Insn *in)
{
	const int sz = in->osz;
	uint32_t lin = 0, a = RMW_READ(&in->a, sz, lin);
	uint32_t r = alu_sub(0, a, 0, sz, in->fmask);
	RMW_WRITE(&in->a, sz, lin, r);
}

ALWAYS_INLINE void exec_not(const Insn *in)
{
	const int sz = in->osz;
	uint32_t lin = 0, a = RMW_READ(&in->a, sz, lin);
	RMW_WRITE(&in->a, sz, lin, ~a & sz_mask(sz));
}

ALWAYS_INLINE void exec_mul(const Insn *in)
{
	alu_mul1(in->op == OP_IMUL1, opnd_read(&in->a, in->osz), in->osz, in->fmask);
}

/* two/three operand IMUL: a = b * c */
ALWAYS_INLINE void exec_imul(const Insn *in)
{
	const int sz = in->osz;
	uint32_t b = opnd_read(&in->b, sz), c = opnd_read(&in->c, sz);
	reg_set(in->a.reg, alu_imul(b, c, sz, in->fmask), sz);
}

/* returns 1 on divide error */
ALWAYS_INLINE int exec_div(const Insn *in)
{
	return alu_div(in->op == OP_IDIV, opnd_read(&in->a, in->osz), in->osz);
}

/* ROL ROR RCL RCR SHL SHR SAL SAR: a by b (286+: count masked to 5 bits) */
ALWAYS_INLINE void exec_shift(const Insn *in)
{
	const int sz = in->osz;
	uint32_t lin = 0, a, r, count = opnd_read(&in->b, 1) & 0x1F;

	if (count == 0)
		return;
	a = RMW_READ(&in->a, sz, lin);
	r = alu_shift(in->op, a, count, sz, in->fmask);
	RMW_WRITE(&in->a, sz, lin, r);
}

ALWAYS_INLINE void exec_shxd(const Insn *in)
{
	const int sz = in->osz, bits = sz * 8;
	uint32_t lin = 0, a, b = opnd_read(&in->b, sz), r, count = opnd_read(&in->c, 1) & 0x1F;
	uint64_t t;

	if (count == 0)
		return;
	a = RMW_READ(&in->a, sz, lin);
	if (in->op == OP_SHLD) {
		t = ((uint64_t)a << bits) | b;
		cpu.cf = (uint8_t)((t >> (2 * bits - count)) & 1);
		r = (uint32_t)((t << count) >> bits) & sz_mask(sz);
	} else {
		t = ((uint64_t)b << bits) | a;
		cpu.cf = (uint8_t)((t >> (count - 1)) & 1);
		r = (uint32_t)(t >> count) & sz_mask(sz);
	}
	cpu.of = ((r ^ a) & sz_sign(sz)) != 0;
	set_szp(r, sz, FM_ALL);
	RMW_WRITE(&in->a, sz, lin, r);
}

ALWAYS_INLINE void exec_bt(const Insn *in)
{
	const int sz = in->osz;
	uint32_t lin = 0, a, bit = opnd_read(&in->b, sz) & (sz * 8 - 1), r;

	/* memory forms with a register bit offset can address outside the operand */
	if (in->a.type == OT_MEM && in->b.type == OT_REG) {
		int32_t off = sz == 2 ? (int16_t)opnd_read(&in->b, 2) : (int32_t)opnd_read(&in->b, 4);
		lin = ea_linear(&in->a) + (uint32_t)((off >> 3) & ~(sz - 1));
		bit = (uint32_t)off & (sz * 8 - 1);
		a = rdn(lin, sz);
	} else {
		a = RMW_READ(&in->a, sz, lin);
	}
	cpu.cf = (a >> bit) & 1;
	switch (in->op) {
	case OP_BTS: r = a | (1u << bit); break;
	case OP_BTR: r = a & ~(1u << bit); break;
	case OP_BTC: r = a ^ (1u << bit); break;
	default: return;
	}
	RMW_WRITE(&in->a, sz, lin, r);
}

ALWAYS_INLINE void exec_bsx(const Insn *in)
{
	const int sz = in->osz;
	uint32_t v = opnd_read(&in->b, sz);

	if (v == 0) {
		cpu.zf = 1;
		return;
	}
	cpu.zf = 0;
	reg_set(in->a.reg, in->op == OP_BSF ? (uint32_t)__builtin_ctz(v) : (uint32_t)(31 - __builtin_clz(v)), sz);
}

ALWAYS_INLINE int exec_aam(const Insn *in)
{
	uint8_t base = (uint8_t)in->a.imm;
	if (base == 0)
		return 1;
	rAH = rAL / base;
	rAL = rAL % base;
	set_szp(rAL, 1, FM_ALL);
	cpu.cf = cpu.of = cpu.af = 0;
	return 0;
}

ALWAYS_INLINE void exec_aad(const Insn *in)
{
	rAL = (uint8_t)(rAL + rAH * (uint8_t)in->a.imm);
	rAH = 0;
	set_szp(rAL, 1, FM_ALL);
	cpu.cf = cpu.of = cpu.af = 0;
}

/* ---- data movement ----------------------------------------------------------------- */

ALWAYS_INLINE void exec_mov(const Insn *in)
{
	opnd_write(&in->a, opnd_read(&in->b, in->osz), in->osz);
}

ALWAYS_INLINE void exec_movx(const Insn *in)
{
	uint32_t v = opnd_read(&in->b, in->b.size);
	if (in->op == OP_MOVSX)
		v = in->b.size == 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
	reg_set(in->a.reg, v, in->osz);
}

ALWAYS_INLINE void exec_xchg(const Insn *in)
{
	const int sz = in->osz;
	uint32_t lin = 0, a = RMW_READ(&in->a, sz, lin), b = reg_get(in->b.reg, sz);
	reg_set(in->b.reg, a, sz);
	RMW_WRITE(&in->a, sz, lin, b);
}

ALWAYS_INLINE void exec_lea(const Insn *in)
{
	reg_set(in->a.reg, ea_offset(&in->b), in->osz);
}

/* LDS LES LSS LFS LGS */
ALWAYS_INLINE void exec_lxs(const Insn *in)
{
	uint32_t a = ea_linear(&in->b);
	uint32_t off = in->osz == 2 ? rd16(a) : rd32(a);
	uint16_t seg = rd16(a + in->osz);
	int s = in->op == OP_LDS ? S_DS : in->op == OP_LES ? S_ES : in->op == OP_LSS ? S_SS :
	        in->op == OP_LFS ? S_FS : S_GS;
	reg_set(in->a.reg, off, in->osz);
	cpu.s[s] = seg;
}

ALWAYS_INLINE void exec_push(const Insn *in)
{
	uint32_t v;
	if (in->a.type == OT_REG && in->a.reg == R_SP && in->osz == 2)
		v = rSP; /* 286+: the value before the push */
	else
		v = opnd_read(&in->a, in->a.type == OT_SREG ? 2 : in->osz);
	if (in->osz == 4)
		push32(v);
	else
		push16((uint16_t)v);
}

ALWAYS_INLINE void exec_pop(const Insn *in)
{
	if (in->osz == 4) {
		uint32_t v = pop32();
		opnd_write(&in->a, v, in->a.type == OT_SREG ? 2 : 4);
	} else {
		uint16_t v = pop16();
		/* POP [mem] with SP-relative addressing uses the incremented SP */
		opnd_write(&in->a, v, 2);
	}
}

ALWAYS_INLINE void exec_pusha(const Insn *in)
{
	if (in->osz == 4) {
		uint32_t sp = cpu.r[R_SP].d;
		int i;
		for (i = 0; i < 8; i++)
			push32(i == R_SP ? sp : cpu.r[i].d);
	} else {
		uint16_t sp = rSP;
		int i;
		for (i = 0; i < 8; i++)
			push16(i == R_SP ? sp : cpu.r[i].w);
	}
}

ALWAYS_INLINE void exec_popa(const Insn *in)
{
	int i;
	for (i = 7; i >= 0; i--) {
		if (in->osz == 4) {
			uint32_t v = pop32();
			if (i != R_SP)
				cpu.r[i].d = v;
		} else {
			uint16_t v = pop16();
			if (i != R_SP)
				cpu.r[i].w = v;
		}
	}
}

ALWAYS_INLINE void exec_pushf(const Insn *in)
{
	if (in->osz == 4)
		push32(flags_get());
	else
		push16(flags_get());
}

ALWAYS_INLINE void exec_popf(const Insn *in)
{
	flags_set(in->osz == 4 ? (uint16_t)pop32() : pop16());
}

ALWAYS_INLINE void exec_lahf(void)
{
	rAH = (uint8_t)flags_get();
}

ALWAYS_INLINE void exec_sahf(void)
{
	flags_set_status(rAH);
}

ALWAYS_INLINE void exec_cbw(const Insn *in)
{
	if (in->osz == 4)
		cpu.r[R_AX].d = (uint32_t)(int32_t)(int16_t)rAX;
	else
		rAX = (uint16_t)(int16_t)(int8_t)rAL;
}

ALWAYS_INLINE void exec_cwd(const Insn *in)
{
	if (in->osz == 4)
		cpu.r[R_DX].d = (cpu.r[R_AX].d & 0x80000000u) ? 0xFFFFFFFFu : 0;
	else
		rDX = (rAX & 0x8000) ? 0xFFFF : 0;
}

ALWAYS_INLINE void exec_xlat(const Insn *in)
{
	rAL = rd8(LIN(cpu.s[in->seg], rBX + rAL));
}

ALWAYS_INLINE void exec_setcc(const Insn *in)
{
	opnd_write(&in->a, (uint32_t)cond(in->cc), 1);
}

ALWAYS_INLINE void exec_enter(const Insn *in)
{
	uint16_t size = (uint16_t)in->a.imm;
	int level = in->b.imm & 31;
	uint16_t frame;

	push16(rBP);
	frame = rSP;
	if (level > 0) {
		int i;
		for (i = 1; i < level; i++) {
			rBP -= 2;
			push16(rd16(LIN(sSS, rBP)));
		}
		push16(frame);
	}
	rBP = frame;
	rSP -= size;
}

ALWAYS_INLINE void exec_leave(void)
{
	rSP = rBP;
	rBP = pop16();
}

/* returns 1 if the index is out of bounds (#BR, INT 5) */
ALWAYS_INLINE int exec_bound(const Insn *in)
{
	uint32_t a = ea_linear(&in->b);
	int16_t v = (int16_t)reg_get(in->a.reg, 2), lo = (int16_t)rd16(a), hi = (int16_t)rd16(a + 2);
	return v < lo || v > hi;
}

ALWAYS_INLINE void exec_in(const Insn *in)
{
	uint16_t port = in->b.type == OT_IMM ? (uint16_t)in->b.imm : rDX;
	reg_set(R_AX, port_in(port, in->osz), in->osz);
}

ALWAYS_INLINE void exec_out(const Insn *in)
{
	uint16_t port = in->b.type == OT_IMM ? (uint16_t)in->b.imm : rDX;
	port_out(port, reg_get(R_AX, in->osz), in->osz);
}

ALWAYS_INLINE void exec_flagop(const Insn *in)
{
	switch (in->op) {
	case OP_CLC: cpu.cf = 0; break;
	case OP_STC: cpu.cf = 1; break;
	case OP_CMC: cpu.cf ^= 1; break;
	case OP_CLD: cpu.df = 0; break;
	case OP_STD: cpu.df = 1; break;
	case OP_CLI: cpu.ifl = 0; break;
	default: cpu.ifl = 1; break; /* STI */
	}
}

ALWAYS_INLINE void exec_salc(void)
{
	rAL = cpu.cf ? 0xFF : 0;
}

ALWAYS_INLINE void exec_smsw(const Insn *in)
{
	opnd_write(&in->a, 0xFFF0, 2); /* real mode: PE = 0 (a 286 reads the reserved bits as 1) */
}

ALWAYS_INLINE void exec_string(const Insn *in)
{
	switch (in->op) {
	case OP_MOVS: str_movs(in->osz, in->seg, in->rep); break;
	case OP_STOS: str_stos(in->osz, in->rep); break;
	case OP_LODS: str_lods(in->osz, in->seg, in->rep); break;
	case OP_CMPS: str_cmps(in->osz, in->seg, in->rep); break;
	case OP_SCAS: str_scas(in->osz, in->rep); break;
	case OP_INS: str_ins(in->osz, in->rep); break;
	default: str_outs(in->osz, in->seg, in->rep); break;
	}
}

#endif
