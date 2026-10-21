/*
 * The 16-bit x86 machine state shared by the interpreter, the statically
 * recompiled game code and the DOS/BIOS/VGA emulation.
 *
 * Real mode only: linear address = segment * 16 + offset, 1 MB + 64 KB of
 * memory (no A20 wrap). The VGA window (A0000-BFFFF) is trapped and handled
 * by vga.c; everything else is plain RAM, including the fake ROMs.
 *
 * Flags are kept unpacked: CF/ZF/SF/OF/DF/IF/TF as 0/1 bytes, PF as the low
 * byte of the last result (parity is computed when read) and AF as bit 4 of
 * a ^ b ^ result.
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <string.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ALWAYS_INLINE static inline __attribute__((always_inline))

typedef union {
	uint32_t d;
	uint16_t w;
	struct {
		uint8_t l, h;
	} b;
} Reg;

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

typedef struct {
	Reg r[8];
	uint16_t s[6];
	uint16_t ip;
	uint8_t cf, zf, sf, of, pfv, af, df, ifl, tf;
	uint8_t halted;
	/* instructions completed; drives virtual time and event polling */
	uint64_t icount;
	/* the next icount at which machine_event() must run */
	uint64_t deadline;
	/* compiled code gave up (see machine.c, "bail") */
	int bail;
} Cpu;

extern Cpu cpu;

#define MEM_SIZE 0x110000u
extern uint8_t mem[MEM_SIZE + 16];

/* register shorthands used by ops.h and the generated code */
#define rAX cpu.r[R_AX].w
#define rCX cpu.r[R_CX].w
#define rDX cpu.r[R_DX].w
#define rBX cpu.r[R_BX].w
#define rSP cpu.r[R_SP].w
#define rBP cpu.r[R_BP].w
#define rSI cpu.r[R_SI].w
#define rDI cpu.r[R_DI].w
#define rAL cpu.r[R_AX].b.l
#define rAH cpu.r[R_AX].b.h
#define rCL cpu.r[R_CX].b.l
#define rCH cpu.r[R_CX].b.h
#define rDL cpu.r[R_DX].b.l
#define rDH cpu.r[R_DX].b.h
#define rBL cpu.r[R_BX].b.l
#define rBH cpu.r[R_BX].b.h
#define sES cpu.s[S_ES]
#define sCS cpu.s[S_CS]
#define sSS cpu.s[S_SS]
#define sDS cpu.s[S_DS]

#define LIN(seg, off) ((((uint32_t)(seg)) << 4) + (uint32_t)(uint16_t)(off))

/* ---- memory --------------------------------------------------------------- */

uint8_t vga_mem_read8(uint32_t addr);
void vga_mem_write8(uint32_t addr, uint8_t v);

/* first byte of a multi-byte access that may touch the VGA window */
#define VGA_SPAN(a, n) ((uint32_t)(a) - (0xA0000u - ((n) - 1)) < 0x20000u + ((n) - 1))

ALWAYS_INLINE uint8_t rd8(uint32_t a)
{
	if (unlikely(a - 0xA0000u < 0x20000u))
		return vga_mem_read8(a);
	return mem[a];
}

ALWAYS_INLINE void wr8(uint32_t a, uint8_t v)
{
	if (unlikely(a - 0xA0000u < 0x20000u)) {
		vga_mem_write8(a, v);
		return;
	}
	mem[a] = v;
}

/* multi-byte accesses touching the VGA window, out of line (ops.c) */
uint16_t vga_rd16(uint32_t a);
void vga_wr16(uint32_t a, uint16_t v);
uint32_t vga_rd32(uint32_t a);
void vga_wr32(uint32_t a, uint32_t v);

ALWAYS_INLINE uint16_t rd16(uint32_t a)
{
	uint16_t v;
	if (unlikely(VGA_SPAN(a, 2)))
		return vga_rd16(a);
	memcpy(&v, mem + a, 2);
	return v;
}

ALWAYS_INLINE void wr16(uint32_t a, uint16_t v)
{
	if (unlikely(VGA_SPAN(a, 2))) {
		vga_wr16(a, v);
		return;
	}
	memcpy(mem + a, &v, 2);
}

ALWAYS_INLINE uint32_t rd32(uint32_t a)
{
	uint32_t v;
	if (unlikely(VGA_SPAN(a, 4)))
		return vga_rd32(a);
	memcpy(&v, mem + a, 4);
	return v;
}

ALWAYS_INLINE void wr32(uint32_t a, uint32_t v)
{
	if (unlikely(VGA_SPAN(a, 4))) {
		vga_wr32(a, v);
		return;
	}
	memcpy(mem + a, &v, 4);
}

ALWAYS_INLINE uint32_t rdn(uint32_t a, int size)
{
	return size == 1 ? rd8(a) : size == 2 ? rd16(a) : rd32(a);
}

ALWAYS_INLINE void wrn(uint32_t a, uint32_t v, int size)
{
	if (size == 1)
		wr8(a, (uint8_t)v);
	else if (size == 2)
		wr16(a, (uint16_t)v);
	else
		wr32(a, v);
}

/* ---- registers by number ---------------------------------------------------- */

/* 8-bit register numbers: 0-3 = AL CL DL BL, 4-7 = AH CH DH BH */
ALWAYS_INLINE uint32_t reg_get(int r, int size)
{
	if (size == 1)
		return r < 4 ? cpu.r[r].b.l : cpu.r[r - 4].b.h;
	if (size == 2)
		return cpu.r[r].w;
	return cpu.r[r].d;
}

ALWAYS_INLINE void reg_set(int r, uint32_t v, int size)
{
	if (size == 1) {
		if (r < 4)
			cpu.r[r].b.l = (uint8_t)v;
		else
			cpu.r[r - 4].b.h = (uint8_t)v;
	} else if (size == 2) {
		cpu.r[r].w = (uint16_t)v;
	} else {
		cpu.r[r].d = v;
	}
}

/* ---- stack ------------------------------------------------------------------ */

/* The stack never lives in the VGA window: SS-relative accesses skip the
 * check (rds16/wrs16 are also used by the recompiled code for [bp+x]). */
ALWAYS_INLINE uint8_t rds8(uint32_t a)
{
	return mem[a];
}

ALWAYS_INLINE void wrs8(uint32_t a, uint8_t v)
{
	mem[a] = v;
}

ALWAYS_INLINE uint16_t rds16(uint32_t a)
{
	uint16_t v;
	memcpy(&v, mem + a, 2);
	return v;
}

ALWAYS_INLINE void wrs16(uint32_t a, uint16_t v)
{
	memcpy(mem + a, &v, 2);
}

ALWAYS_INLINE uint32_t rds32(uint32_t a)
{
	uint32_t v;
	memcpy(&v, mem + a, 4);
	return v;
}

ALWAYS_INLINE void wrs32(uint32_t a, uint32_t v)
{
	memcpy(mem + a, &v, 4);
}

ALWAYS_INLINE void push16(uint16_t v)
{
	rSP -= 2;
	wrs16(LIN(sSS, rSP), v);
}

ALWAYS_INLINE uint16_t pop16(void)
{
	uint16_t v = rds16(LIN(sSS, rSP));
	rSP += 2;
	return v;
}

ALWAYS_INLINE void push32(uint32_t v)
{
	rSP -= 4;
	wrs32(LIN(sSS, rSP), v);
}

ALWAYS_INLINE uint32_t pop32(void)
{
	uint32_t v = rds32(LIN(sSS, rSP));
	rSP += 4;
	return v;
}

/* ---- flags ------------------------------------------------------------------ */

extern const uint8_t parity_table[256];

#define FLAG_CF 0x0001
#define FLAG_PF 0x0004
#define FLAG_AF 0x0010
#define FLAG_ZF 0x0040
#define FLAG_SF 0x0080
#define FLAG_TF 0x0100
#define FLAG_IF 0x0200
#define FLAG_DF 0x0400
#define FLAG_OF 0x0800

ALWAYS_INLINE int get_pf(void)
{
	return parity_table[cpu.pfv];
}

/* the FLAGS register as a 286 in real mode shows it: bits 12-15 read 0 */
ALWAYS_INLINE uint16_t flags_get(void)
{
	return (uint16_t)(cpu.cf | 0x0002 | (get_pf() << 2) | (cpu.af ? FLAG_AF : 0) |
	                  (cpu.zf << 6) | (cpu.sf << 7) | (cpu.tf << 8) | (cpu.ifl << 9) |
	                  (cpu.df << 10) | (cpu.of << 11));
}

ALWAYS_INLINE void flags_set_status(uint16_t f)
{
	cpu.cf = f & 1;
	cpu.pfv = (f & FLAG_PF) ? 0 : 1; /* parity_table[0] = 1, [1] = 0 */
	cpu.af = (f & FLAG_AF) ? 0x10 : 0;
	cpu.zf = (f >> 6) & 1;
	cpu.sf = (f >> 7) & 1;
}

ALWAYS_INLINE void flags_set(uint16_t f)
{
	flags_set_status(f);
	cpu.tf = (f >> 8) & 1;
	cpu.ifl = (f >> 9) & 1;
	cpu.df = (f >> 10) & 1;
	cpu.of = (f >> 11) & 1;
}

#endif
