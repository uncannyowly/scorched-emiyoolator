/*
 * Out-of-line instruction semantics: string instructions and BCD.
 */
#include "ops.h"

Cpu cpu;
uint8_t mem[MEM_SIZE + 16] __attribute__((aligned(64)));

const uint8_t parity_table[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
	P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

uint16_t vga_rd16(uint32_t a)
{
	return (uint16_t)(rd8(a) | (rd8(a + 1) << 8));
}

void vga_wr16(uint32_t a, uint16_t v)
{
	wr8(a, (uint8_t)v);
	wr8(a + 1, (uint8_t)(v >> 8));
}

uint32_t vga_rd32(uint32_t a)
{
	return vga_rd16(a) | ((uint32_t)vga_rd16(a + 2) << 16);
}

void vga_wr32(uint32_t a, uint32_t v)
{
	vga_wr16(a, (uint16_t)v);
	vga_wr16(a + 2, (uint16_t)(v >> 16));
}

/* element-wise, so overlapping copies replicate the way the CPU does */
void str_movs(int size, int seg, int rep)
{
	int step = cpu.df ? -size : size;
	uint16_t sseg = cpu.s[seg];

	if (!rep) {
		wrn(LIN(sES, rDI), rdn(LIN(sseg, rSI), size), size);
		rSI += step;
		rDI += step;
		return;
	}
	while (rCX) {
		wrn(LIN(sES, rDI), rdn(LIN(sseg, rSI), size), size);
		rSI += step;
		rDI += step;
		rCX--;
	}
}

void str_stos(int size, int rep)
{
	int step = cpu.df ? -size : size;
	uint32_t v = reg_get(R_AX, size);

	if (!rep) {
		wrn(LIN(sES, rDI), v, size);
		rDI += step;
		return;
	}
	/* fast path: forward fill of plain RAM within one segment */
	if (step > 0 && size <= 2) {
		uint32_t bytes = (uint32_t)rCX * size, a = LIN(sES, rDI);
		if ((uint32_t)rDI + bytes <= 0x10000u && a + bytes <= 0xA0000u) {
			if (size == 1) {
				memset(mem + a, (int)v, bytes);
			} else {
				uint32_t i;
				for (i = 0; i < bytes; i += 2) {
					mem[a + i] = (uint8_t)v;
					mem[a + i + 1] = (uint8_t)(v >> 8);
				}
			}
			rDI = (uint16_t)(rDI + bytes);
			rCX = 0;
			return;
		}
	}
	while (rCX) {
		wrn(LIN(sES, rDI), v, size);
		rDI += step;
		rCX--;
	}
}

void str_lods(int size, int seg, int rep)
{
	int step = cpu.df ? -size : size;
	uint16_t sseg = cpu.s[seg];

	if (!rep) {
		reg_set(R_AX, rdn(LIN(sseg, rSI), size), size);
		rSI += step;
		return;
	}
	while (rCX) {
		reg_set(R_AX, rdn(LIN(sseg, rSI), size), size);
		rSI += step;
		rCX--;
	}
}

static void cmp_flags(uint32_t a, uint32_t b, int size)
{
	alu_sub(a, b, 0, size, FM_ALL);
}

/* rep = 0xF3: repeat while equal; 0xF2: while not equal */
void str_cmps(int size, int seg, int rep)
{
	int step = cpu.df ? -size : size;
	uint16_t sseg = cpu.s[seg];

	if (!rep) {
		cmp_flags(rdn(LIN(sseg, rSI), size), rdn(LIN(sES, rDI), size), size);
		rSI += step;
		rDI += step;
		return;
	}
	while (rCX) {
		cmp_flags(rdn(LIN(sseg, rSI), size), rdn(LIN(sES, rDI), size), size);
		rSI += step;
		rDI += step;
		rCX--;
		if ((rep == 0xF3) != cpu.zf)
			break;
	}
}

void str_scas(int size, int rep)
{
	int step = cpu.df ? -size : size;
	uint32_t v = reg_get(R_AX, size);

	if (!rep) {
		cmp_flags(v, rdn(LIN(sES, rDI), size), size);
		rDI += step;
		return;
	}
	while (rCX) {
		cmp_flags(v, rdn(LIN(sES, rDI), size), size);
		rDI += step;
		rCX--;
		if ((rep == 0xF3) != cpu.zf)
			break;
	}
}

void str_ins(int size, int rep)
{
	int step = cpu.df ? -size : size;

	do {
		if (rep && !rCX)
			break;
		wrn(LIN(sES, rDI), port_in(rDX, size), size);
		rDI += step;
		if (rep)
			rCX--;
	} while (rep);
}

void str_outs(int size, int seg, int rep)
{
	int step = cpu.df ? -size : size;
	uint16_t sseg = cpu.s[seg];

	do {
		if (rep && !rCX)
			break;
		port_out(rDX, rdn(LIN(sseg, rSI), size), size);
		rSI += step;
		if (rep)
			rCX--;
	} while (rep);
}

void bcd_daa(void)
{
	uint8_t al = rAL, cf = cpu.cf;

	cpu.cf = 0;
	if ((al & 0x0F) > 9 || cpu.af) {
		rAL = (uint8_t)(al + 6);
		cpu.cf = cf || al > 0xF9;
		cpu.af = 0x10;
	} else {
		cpu.af = 0;
	}
	if (al > 0x99 || cf) {
		rAL += 0x60;
		cpu.cf = 1;
	}
	set_szp(rAL, 1, FM_ALL);
}

void bcd_das(void)
{
	uint8_t al = rAL, cf = cpu.cf;

	cpu.cf = 0;
	if ((al & 0x0F) > 9 || cpu.af) {
		rAL = (uint8_t)(al - 6);
		cpu.cf = cf || al < 6;
		cpu.af = 0x10;
	} else {
		cpu.af = 0;
	}
	if (al > 0x99 || cf) {
		rAL -= 0x60;
		cpu.cf = 1;
	}
	set_szp(rAL, 1, FM_ALL);
}

void bcd_aaa(void)
{
	if ((rAL & 0x0F) > 9 || cpu.af) {
		rAX = (uint16_t)(rAX + 0x106);
		cpu.af = 0x10;
		cpu.cf = 1;
	} else {
		cpu.af = 0;
		cpu.cf = 0;
	}
	rAL &= 0x0F;
}

void bcd_aas(void)
{
	if ((rAL & 0x0F) > 9 || cpu.af) {
		rAX = (uint16_t)(rAX - 6);
		rAH -= 1;
		cpu.af = 0x10;
		cpu.cf = 1;
	} else {
		cpu.af = 0;
		cpu.cf = 0;
	}
	rAL &= 0x0F;
}
