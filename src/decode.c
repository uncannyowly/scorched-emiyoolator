/*
 * x86 real-mode instruction decoder. See insn.h.
 */
#include "insn.h"
#include "cpu.h"

#include <stdio.h>
#include <string.h>

const char *const op_names[OP_COUNT] = {
	[OP_BAD] = "(bad)",
	[OP_ADD] = "add", [OP_OR] = "or", [OP_ADC] = "adc", [OP_SBB] = "sbb",
	[OP_AND] = "and", [OP_SUB] = "sub", [OP_XOR] = "xor", [OP_CMP] = "cmp",
	[OP_TEST] = "test", [OP_INC] = "inc", [OP_DEC] = "dec", [OP_NEG] = "neg",
	[OP_NOT] = "not", [OP_MUL] = "mul", [OP_IMUL1] = "imul", [OP_IMUL] = "imul",
	[OP_DIV] = "div", [OP_IDIV] = "idiv",
	[OP_ROL] = "rol", [OP_ROR] = "ror", [OP_RCL] = "rcl", [OP_RCR] = "rcr",
	[OP_SHL] = "shl", [OP_SHR] = "shr", [OP_SAL] = "sal", [OP_SAR] = "sar",
	[OP_SHLD] = "shld", [OP_SHRD] = "shrd",
	[OP_DAA] = "daa", [OP_DAS] = "das", [OP_AAA] = "aaa", [OP_AAS] = "aas",
	[OP_AAM] = "aam", [OP_AAD] = "aad", [OP_SALC] = "salc",
	[OP_BT] = "bt", [OP_BTS] = "bts", [OP_BTR] = "btr", [OP_BTC] = "btc",
	[OP_BSF] = "bsf", [OP_BSR] = "bsr",
	[OP_MOV] = "mov", [OP_MOVZX] = "movzx", [OP_MOVSX] = "movsx", [OP_XCHG] = "xchg",
	[OP_LEA] = "lea", [OP_LDS] = "lds", [OP_LES] = "les", [OP_LSS] = "lss",
	[OP_LFS] = "lfs", [OP_LGS] = "lgs",
	[OP_PUSH] = "push", [OP_POP] = "pop", [OP_PUSHA] = "pusha", [OP_POPA] = "popa",
	[OP_PUSHF] = "pushf", [OP_POPF] = "popf", [OP_LAHF] = "lahf", [OP_SAHF] = "sahf",
	[OP_CBW] = "cbw", [OP_CWD] = "cwd", [OP_XLAT] = "xlat", [OP_SETCC] = "set",
	[OP_IN] = "in", [OP_OUT] = "out", [OP_ENTER] = "enter", [OP_LEAVE] = "leave",
	[OP_BOUND] = "bound",
	[OP_CLC] = "clc", [OP_STC] = "stc", [OP_CMC] = "cmc", [OP_CLD] = "cld",
	[OP_STD] = "std", [OP_CLI] = "cli", [OP_STI] = "sti",
	[OP_NOP] = "nop", [OP_WAIT] = "wait", [OP_SMSW] = "smsw",
	[OP_MOVS] = "movs", [OP_CMPS] = "cmps", [OP_STOS] = "stos", [OP_LODS] = "lods",
	[OP_SCAS] = "scas", [OP_INS] = "ins", [OP_OUTS] = "outs",
	[OP_JMP] = "jmp", [OP_JCC] = "j", [OP_JCXZ] = "jcxz", [OP_LOOP] = "loop",
	[OP_LOOPE] = "loope", [OP_LOOPNE] = "loopne", [OP_JMPF] = "jmp far",
	[OP_JMP_IND] = "jmp", [OP_JMPF_IND] = "jmp far", [OP_CALL] = "call",
	[OP_CALLF] = "call far", [OP_CALL_IND] = "call", [OP_CALLF_IND] = "call far",
	[OP_RET] = "ret", [OP_RETF] = "retf", [OP_IRET] = "iret", [OP_INT] = "int",
	[OP_INTO] = "into", [OP_HLT] = "hlt",
	[OP_FPU] = "fpu", [OP_FPU_SHORTCUT] = "fpu-shortcut",
};

static const char *const cc_names[16] = {
	"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"
};

typedef struct {
	const uint8_t *p;
	int avail;
	int pos;
	int bad;
} Stream;

static uint8_t get8(Stream *s)
{
	if (s->pos >= s->avail) {
		s->bad = 1;
		return 0;
	}
	return s->p[s->pos++];
}

static uint16_t get16(Stream *s)
{
	uint16_t lo = get8(s);
	return (uint16_t)(lo | (get8(s) << 8));
}

static uint32_t get32(Stream *s)
{
	uint32_t lo = get16(s);
	return lo | ((uint32_t)get16(s) << 16);
}

static uint32_t get_imm(Stream *s, int size)
{
	return size == 1 ? get8(s) : size == 2 ? get16(s) : get32(s);
}

static void set_reg(Operand *o, int reg, int size)
{
	o->type = OT_REG;
	o->reg = (uint8_t)reg;
	o->size = (uint8_t)size;
}

static void set_imm(Operand *o, uint32_t v, int size)
{
	o->type = OT_IMM;
	o->imm = v;
	o->size = (uint8_t)size;
}

/* modrm with the r/m operand into `rm`; returns the reg field. seg_ovr < 0 = none */
static int modrm(Stream *s, Operand *rm, int size, int seg_ovr, uint8_t *raw)
{
	uint8_t m = get8(s);
	int mod = m >> 6, reg = (m >> 3) & 7, r = m & 7;

	if (raw)
		*raw = m;
	if (mod == 3) {
		set_reg(rm, r, size);
		return reg;
	}
	rm->type = OT_MEM;
	rm->size = (uint8_t)size;
	if (mod == 0 && r == 6) {
		rm->ea = EA_DISP;
		rm->disp = get16(s);
	} else {
		rm->ea = (uint8_t)r;
		if (mod == 1)
			rm->disp = (uint16_t)(int16_t)(int8_t)get8(s);
		else if (mod == 2)
			rm->disp = get16(s);
		else
			rm->disp = 0;
	}
	if (seg_ovr >= 0)
		rm->seg = (uint8_t)seg_ovr;
	else
		rm->seg = (rm->ea == EA_BP_SI || rm->ea == EA_BP_DI || rm->ea == EA_BP) ? S_SS : S_DS;
	return reg;
}

static int fail(Insn *in)
{
	in->op = OP_BAD;
	in->len = 0;
	return 0;
}

int x86_decode(const uint8_t *code, int avail, uint16_t ip, Insn *in)
{
	Stream st = { code, avail > 15 ? 15 : avail, 0, 0 };
	Stream *s = &st;
	int seg_ovr = -1, opsize = 2, addr32 = 0;
	uint8_t b;
	int reg;

	memset(in, 0, sizeof(*in));
	in->seg = S_DS;
	in->fmask = FM_ALL;

	for (;;) {
		b = get8(s);
		if (s->bad)
			return fail(in);
		switch (b) {
		case 0x26: seg_ovr = S_ES; continue;
		case 0x2E: seg_ovr = S_CS; continue;
		case 0x36: seg_ovr = S_SS; continue;
		case 0x3E: seg_ovr = S_DS; continue;
		case 0x64: seg_ovr = S_FS; continue;
		case 0x65: seg_ovr = S_GS; continue;
		case 0x66: opsize = 4; in->flags |= IF_386; continue;
		case 0x67: addr32 = 1; continue;
		case 0xF0: in->flags |= IF_LOCK; continue;
		case 0xF2: case 0xF3: in->rep = b; continue;
		}
		break;
	}
	if (addr32)
		return fail(in); /* 32-bit addressing is not used by 16-bit DOS code here */
	if (seg_ovr >= 0)
		in->seg = (uint8_t)seg_ovr;
	in->osz = (uint8_t)opsize;

	/* the eight classic ALU ops: 00-3F, pattern xx000ddd */
	if (b < 0x40 && (b & 7) < 6) {
		static const uint16_t alu[8] = { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };
		int sz = (b & 1) ? opsize : 1;
		in->op = alu[b >> 3];
		in->osz = (uint8_t)sz;
		switch (b & 7) {
		case 0: case 1: /* Eb,Gb / Ev,Gv */
			reg = modrm(s, &in->a, sz, seg_ovr, NULL);
			set_reg(&in->b, reg, sz);
			break;
		case 2: case 3: /* Gb,Eb / Gv,Ev */
			reg = modrm(s, &in->b, sz, seg_ovr, NULL);
			set_reg(&in->a, reg, sz);
			break;
		case 4: case 5: /* AL,Ib / eAX,Iv */
			set_reg(&in->a, R_AX, sz);
			set_imm(&in->b, get_imm(s, sz), sz);
			break;
		}
		goto done;
	}

	switch (b) {
	case 0x06: case 0x0E: case 0x16: case 0x1E:
		in->op = OP_PUSH;
		in->a.type = OT_SREG;
		in->a.reg = b >> 3;
		in->a.size = 2;
		in->osz = (uint8_t)opsize;
		break;
	case 0x07: case 0x17: case 0x1F:
		in->op = OP_POP;
		in->a.type = OT_SREG;
		in->a.reg = b >> 3;
		in->a.size = 2;
		in->osz = (uint8_t)opsize;
		break;
	case 0x0F:
		b = get8(s);
		in->flags |= IF_386;
		if (b >= 0x80 && b <= 0x8F) {
			uint32_t rel = opsize == 4 ? get32(s) : get16(s);
			in->op = OP_JCC;
			in->cc = b & 15;
			in->a.type = OT_IMM;
			in->a.imm = (uint16_t)(ip + s->pos + rel);
			break;
		}
		if (b >= 0x90 && b <= 0x9F) {
			in->op = OP_SETCC;
			in->cc = b & 15;
			in->osz = 1;
			modrm(s, &in->a, 1, seg_ovr, NULL);
			break;
		}
		switch (b) {
		case 0x01:
			reg = modrm(s, &in->a, 2, seg_ovr, NULL);
			if (reg != 4)
				return fail(in);
			in->op = OP_SMSW;
			in->osz = 2;
			break;
		case 0xA0: case 0xA8:
			in->op = OP_PUSH;
			in->a.type = OT_SREG;
			in->a.reg = b == 0xA0 ? S_FS : S_GS;
			in->a.size = 2;
			break;
		case 0xA1: case 0xA9:
			in->op = OP_POP;
			in->a.type = OT_SREG;
			in->a.reg = b == 0xA1 ? S_FS : S_GS;
			in->a.size = 2;
			break;
		case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
			static const uint16_t bt[4] = { OP_BT, OP_BTS, OP_BTR, OP_BTC };
			in->op = bt[(b >> 3) & 3];
			reg = modrm(s, &in->a, opsize, seg_ovr, NULL);
			set_reg(&in->b, reg, opsize);
			break;
		}
		case 0xBA: {
			static const uint16_t bt[4] = { OP_BT, OP_BTS, OP_BTR, OP_BTC };
			reg = modrm(s, &in->a, opsize, seg_ovr, NULL);
			if (reg < 4)
				return fail(in);
			in->op = bt[reg - 4];
			set_imm(&in->b, get8(s), 1);
			break;
		}
		case 0xA4: case 0xA5: case 0xAC: case 0xAD:
			in->op = b < 0xA8 ? OP_SHLD : OP_SHRD;
			reg = modrm(s, &in->a, opsize, seg_ovr, NULL);
			set_reg(&in->b, reg, opsize);
			if (b & 1)
				set_reg(&in->c, R_CX, 1);
			else
				set_imm(&in->c, get8(s), 1);
			break;
		case 0xAF:
			in->op = OP_IMUL;
			reg = modrm(s, &in->b, opsize, seg_ovr, NULL);
			set_reg(&in->a, reg, opsize);
			in->c = in->a;
			break;
		case 0xB2: case 0xB4: case 0xB5:
			in->op = b == 0xB2 ? OP_LSS : b == 0xB4 ? OP_LFS : OP_LGS;
			reg = modrm(s, &in->b, opsize + 2, seg_ovr, NULL);
			if (in->b.type != OT_MEM)
				return fail(in);
			set_reg(&in->a, reg, opsize);
			break;
		case 0xB6: case 0xB7: case 0xBE: case 0xBF:
			in->op = b < 0xB8 ? OP_MOVZX : OP_MOVSX;
			reg = modrm(s, &in->b, (b & 1) ? 2 : 1, seg_ovr, NULL);
			set_reg(&in->a, reg, opsize);
			break;
		case 0xBC: case 0xBD:
			in->op = b == 0xBC ? OP_BSF : OP_BSR;
			reg = modrm(s, &in->b, opsize, seg_ovr, NULL);
			set_reg(&in->a, reg, opsize);
			break;
		default:
			return fail(in);
		}
		break;
	case 0x27: in->op = OP_DAA; in->osz = 1; break;
	case 0x2F: in->op = OP_DAS; in->osz = 1; break;
	case 0x37: in->op = OP_AAA; in->osz = 1; break;
	case 0x3F: in->op = OP_AAS; in->osz = 1; break;
	case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
		in->op = OP_INC;
		set_reg(&in->a, b & 7, opsize);
		break;
	case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
		in->op = OP_DEC;
		set_reg(&in->a, b & 7, opsize);
		break;
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
		in->op = OP_PUSH;
		set_reg(&in->a, b & 7, opsize);
		break;
	case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		in->op = OP_POP;
		set_reg(&in->a, b & 7, opsize);
		break;
	case 0x60: in->op = OP_PUSHA; break;
	case 0x61: in->op = OP_POPA; break;
	case 0x62:
		in->op = OP_BOUND;
		reg = modrm(s, &in->b, opsize * 2, seg_ovr, NULL);
		if (in->b.type != OT_MEM)
			return fail(in);
		set_reg(&in->a, reg, opsize);
		break;
	case 0x68:
		in->op = OP_PUSH;
		set_imm(&in->a, get_imm(s, opsize), opsize);
		break;
	case 0x6A:
		in->op = OP_PUSH;
		set_imm(&in->a, (uint32_t)(int32_t)(int8_t)get8(s), opsize);
		if (opsize == 2)
			in->a.imm &= 0xFFFF;
		break;
	case 0x69: case 0x6B:
		in->op = OP_IMUL;
		reg = modrm(s, &in->b, opsize, seg_ovr, NULL);
		set_reg(&in->a, reg, opsize);
		if (b == 0x69)
			set_imm(&in->c, get_imm(s, opsize), opsize);
		else
			set_imm(&in->c, (uint32_t)(int32_t)(int8_t)get8(s) & (opsize == 2 ? 0xFFFF : 0xFFFFFFFF), opsize);
		break;
	case 0x6C: case 0x6D:
		in->op = OP_INS;
		in->osz = (b & 1) ? (uint8_t)opsize : 1;
		break;
	case 0x6E: case 0x6F:
		in->op = OP_OUTS;
		in->osz = (b & 1) ? (uint8_t)opsize : 1;
		break;
	case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
		int8_t rel = (int8_t)get8(s);
		in->op = OP_JCC;
		in->cc = b & 15;
		in->a.type = OT_IMM;
		in->a.imm = (uint16_t)(ip + s->pos + rel);
		break;
	}
	case 0x80: case 0x81: case 0x82: case 0x83: {
		static const uint16_t alu[8] = { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };
		int sz = (b & 1) ? opsize : 1;
		in->osz = (uint8_t)sz;
		reg = modrm(s, &in->a, sz, seg_ovr, NULL);
		in->op = alu[reg];
		if (b == 0x83)
			set_imm(&in->b, (uint32_t)(int32_t)(int8_t)get8(s) & (sz == 2 ? 0xFFFF : 0xFFFFFFFF), sz);
		else
			set_imm(&in->b, get_imm(s, sz), sz);
		break;
	}
	case 0x84: case 0x85: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_TEST;
		in->osz = (uint8_t)sz;
		reg = modrm(s, &in->a, sz, seg_ovr, NULL);
		set_reg(&in->b, reg, sz);
		break;
	}
	case 0x86: case 0x87: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_XCHG;
		in->osz = (uint8_t)sz;
		reg = modrm(s, &in->a, sz, seg_ovr, NULL);
		set_reg(&in->b, reg, sz);
		break;
	}
	case 0x88: case 0x89: case 0x8A: case 0x8B: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_MOV;
		in->osz = (uint8_t)sz;
		if (b < 0x8A) {
			reg = modrm(s, &in->a, sz, seg_ovr, NULL);
			set_reg(&in->b, reg, sz);
		} else {
			reg = modrm(s, &in->b, sz, seg_ovr, NULL);
			set_reg(&in->a, reg, sz);
		}
		break;
	}
	case 0x8C:
		in->op = OP_MOV;
		in->osz = 2;
		reg = modrm(s, &in->a, 2, seg_ovr, NULL);
		if (reg > 5)
			return fail(in);
		in->b.type = OT_SREG;
		in->b.reg = (uint8_t)reg;
		in->b.size = 2;
		break;
	case 0x8D:
		in->op = OP_LEA;
		reg = modrm(s, &in->b, opsize, seg_ovr, NULL);
		if (in->b.type != OT_MEM)
			return fail(in);
		set_reg(&in->a, reg, opsize);
		break;
	case 0x8E:
		in->op = OP_MOV;
		in->osz = 2;
		reg = modrm(s, &in->b, 2, seg_ovr, NULL);
		if (reg > 5 || reg == S_CS)
			return fail(in);
		in->a.type = OT_SREG;
		in->a.reg = (uint8_t)reg;
		in->a.size = 2;
		break;
	case 0x8F:
		in->op = OP_POP;
		reg = modrm(s, &in->a, opsize, seg_ovr, NULL);
		if (reg != 0)
			return fail(in);
		break;
	case 0x90:
		in->op = OP_NOP;
		break;
	case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
		in->op = OP_XCHG;
		set_reg(&in->a, R_AX, opsize);
		set_reg(&in->b, b & 7, opsize);
		break;
	case 0x98: in->op = OP_CBW; break;
	case 0x99: in->op = OP_CWD; break;
	case 0x9A:
		in->op = OP_CALLF;
		in->a.type = OT_IMM;
		in->a.imm = get16(s);
		in->a.disp = get16(s);
		break;
	case 0x9B: in->op = OP_WAIT; break;
	case 0x9C: in->op = OP_PUSHF; break;
	case 0x9D: in->op = OP_POPF; break;
	case 0x9E: in->op = OP_SAHF; break;
	case 0x9F: in->op = OP_LAHF; break;
	case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
		int sz = (b & 1) ? opsize : 1;
		Operand m;
		memset(&m, 0, sizeof(m));
		m.type = OT_MEM;
		m.size = (uint8_t)sz;
		m.ea = EA_DISP;
		m.disp = get16(s);
		m.seg = (uint8_t)(seg_ovr >= 0 ? seg_ovr : S_DS);
		in->op = OP_MOV;
		in->osz = (uint8_t)sz;
		if (b < 0xA2) {
			set_reg(&in->a, R_AX, sz);
			in->b = m;
		} else {
			in->a = m;
			set_reg(&in->b, R_AX, sz);
		}
		break;
	}
	case 0xA4: case 0xA5: in->op = OP_MOVS; goto string;
	case 0xA6: case 0xA7: in->op = OP_CMPS; goto string;
	case 0xAA: case 0xAB: in->op = OP_STOS; goto string;
	case 0xAC: case 0xAD: in->op = OP_LODS; goto string;
	case 0xAE: case 0xAF: in->op = OP_SCAS;
	string:
		in->osz = (b & 1) ? (uint8_t)opsize : 1;
		break;
	case 0xA8: case 0xA9: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_TEST;
		in->osz = (uint8_t)sz;
		set_reg(&in->a, R_AX, sz);
		set_imm(&in->b, get_imm(s, sz), sz);
		break;
	}
	case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
		in->op = OP_MOV;
		in->osz = 1;
		set_reg(&in->a, b & 7, 1);
		set_imm(&in->b, get8(s), 1);
		break;
	case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		in->op = OP_MOV;
		set_reg(&in->a, b & 7, opsize);
		set_imm(&in->b, get_imm(s, opsize), opsize);
		break;
	case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
		static const uint16_t sh[8] = { OP_ROL, OP_ROR, OP_RCL, OP_RCR, OP_SHL, OP_SHR, OP_SAL, OP_SAR };
		int sz = (b & 1) ? opsize : 1;
		in->osz = (uint8_t)sz;
		reg = modrm(s, &in->a, sz, seg_ovr, NULL);
		in->op = sh[reg];
		if (b < 0xD0)
			set_imm(&in->b, get8(s), 1);
		else if (b < 0xD2)
			set_imm(&in->b, 1, 1);
		else
			set_reg(&in->b, R_CX, 1);
		break;
	}
	case 0xC2: case 0xCA:
		in->op = b == 0xC2 ? OP_RET : OP_RETF;
		in->a.type = OT_IMM;
		in->a.imm = get16(s);
		break;
	case 0xC3: in->op = OP_RET; break;
	case 0xCB: in->op = OP_RETF; break;
	case 0xC4: case 0xC5:
		in->op = b == 0xC4 ? OP_LES : OP_LDS;
		reg = modrm(s, &in->b, opsize + 2, seg_ovr, NULL);
		if (in->b.type != OT_MEM)
			return fail(in);
		set_reg(&in->a, reg, opsize);
		break;
	case 0xC6: case 0xC7: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_MOV;
		in->osz = (uint8_t)sz;
		reg = modrm(s, &in->a, sz, seg_ovr, NULL);
		if (reg != 0)
			return fail(in);
		set_imm(&in->b, get_imm(s, sz), sz);
		break;
	}
	case 0xC8:
		in->op = OP_ENTER;
		set_imm(&in->a, get16(s), 2);
		set_imm(&in->b, get8(s), 1);
		break;
	case 0xC9: in->op = OP_LEAVE; break;
	case 0xCC:
		in->op = OP_INT;
		set_imm(&in->a, 3, 1);
		break;
	case 0xCD: {
		uint8_t n = get8(s);
		if (n >= 0x34 && n <= 0x3B) {
			/* Borland emulator: INT 34h+k = FWAIT; ESC (D8+k) modrm ... */
			in->op = OP_FPU;
			in->flags |= IF_EMU;
			in->fpu_esc = (uint8_t)(n - 0x34);
			modrm(s, &in->a, 0, seg_ovr, &in->modrm);
		} else if (n == 0x3C) {
			/* segment override form: next byte = ESC with the segment in bits 7-6 */
			static const uint8_t segs[4] = { S_DS, S_CS, S_SS, S_ES };
			uint8_t e = get8(s);
			in->op = OP_FPU;
			in->flags |= IF_EMU;
			in->fpu_esc = e & 7;
			modrm(s, &in->a, 0, segs[e >> 6], &in->modrm);
			if (in->a.type != OT_MEM)
				return fail(in);
		} else if (n == 0x3D) {
			in->op = OP_WAIT;
			in->flags |= IF_EMU;
		} else if (n == 0x3E) {
			uint8_t f = get8(s);
			if (get8(s) != 0x90)
				return fail(in);
			in->op = OP_FPU_SHORTCUT;
			in->flags |= IF_EMU;
			set_imm(&in->a, f, 1);
		} else {
			in->op = OP_INT;
			set_imm(&in->a, n, 1);
		}
		break;
	}
	case 0xCE: in->op = OP_INTO; break;
	case 0xCF: in->op = OP_IRET; break;
	case 0xD4: case 0xD5:
		in->op = b == 0xD4 ? OP_AAM : OP_AAD;
		in->osz = 1;
		set_imm(&in->a, get8(s), 1);
		break;
	case 0xD6: in->op = OP_SALC; in->osz = 1; break;
	case 0xD7: in->op = OP_XLAT; in->osz = 1; break;
	case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
		in->op = OP_FPU;
		in->fpu_esc = b & 7;
		modrm(s, &in->a, 0, seg_ovr, &in->modrm);
		break;
	case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
		int8_t rel = (int8_t)get8(s);
		static const uint16_t lp[4] = { OP_LOOPNE, OP_LOOPE, OP_LOOP, OP_JCXZ };
		in->op = lp[b & 3];
		in->a.type = OT_IMM;
		in->a.imm = (uint16_t)(ip + s->pos + rel);
		break;
	}
	case 0xE4: case 0xE5: case 0xEC: case 0xED: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_IN;
		in->osz = (uint8_t)sz;
		set_reg(&in->a, R_AX, sz);
		if (b < 0xE8)
			set_imm(&in->b, get8(s), 1);
		else
			set_reg(&in->b, R_DX, 2);
		break;
	}
	case 0xE6: case 0xE7: case 0xEE: case 0xEF: {
		int sz = (b & 1) ? opsize : 1;
		in->op = OP_OUT;
		in->osz = (uint8_t)sz;
		set_reg(&in->a, R_AX, sz);
		if (b < 0xE8)
			set_imm(&in->b, get8(s), 1);
		else
			set_reg(&in->b, R_DX, 2);
		break;
	}
	case 0xE8: case 0xE9: {
		uint32_t rel = opsize == 4 ? get32(s) : get16(s);
		in->op = b == 0xE8 ? OP_CALL : OP_JMP;
		in->a.type = OT_IMM;
		in->a.imm = (uint16_t)(ip + s->pos + rel);
		break;
	}
	case 0xEA:
		in->op = OP_JMPF;
		in->a.type = OT_IMM;
		in->a.imm = get16(s);
		in->a.disp = get16(s);
		break;
	case 0xEB: {
		int8_t rel = (int8_t)get8(s);
		in->op = OP_JMP;
		in->a.type = OT_IMM;
		in->a.imm = (uint16_t)(ip + s->pos + rel);
		break;
	}
	case 0xF4: in->op = OP_HLT; break;
	case 0xF5: in->op = OP_CMC; break;
	case 0xF6: case 0xF7: {
		static const uint16_t g3[8] = { OP_TEST, OP_TEST, OP_NOT, OP_NEG, OP_MUL, OP_IMUL1, OP_DIV, OP_IDIV };
		int sz = (b & 1) ? opsize : 1;
		in->osz = (uint8_t)sz;
		reg = modrm(s, &in->a, sz, seg_ovr, NULL);
		in->op = g3[reg];
		if (reg < 2)
			set_imm(&in->b, get_imm(s, sz), sz);
		break;
	}
	case 0xF8: in->op = OP_CLC; break;
	case 0xF9: in->op = OP_STC; break;
	case 0xFA: in->op = OP_CLI; break;
	case 0xFB: in->op = OP_STI; break;
	case 0xFC: in->op = OP_CLD; break;
	case 0xFD: in->op = OP_STD; break;
	case 0xFE:
		in->osz = 1;
		reg = modrm(s, &in->a, 1, seg_ovr, NULL);
		if (reg > 1)
			return fail(in);
		in->op = reg == 0 ? OP_INC : OP_DEC;
		break;
	case 0xFF:
		reg = modrm(s, &in->a, opsize, seg_ovr, NULL);
		switch (reg) {
		case 0: in->op = OP_INC; break;
		case 1: in->op = OP_DEC; break;
		case 2: in->op = OP_CALL_IND; break;
		case 3: in->op = OP_CALLF_IND; break;
		case 4: in->op = OP_JMP_IND; break;
		case 5: in->op = OP_JMPF_IND; break;
		case 6: in->op = OP_PUSH; break;
		default: return fail(in);
		}
		if ((reg == 3 || reg == 5) && in->a.type != OT_MEM)
			return fail(in);
		if (reg == 3 || reg == 5)
			in->a.size = (uint8_t)(opsize + 2);
		break;
	default:
		return fail(in);
	}

done:
	if (s->bad)
		return fail(in);
	/* LOCK only on read-modify-write memory forms; real mode code here never uses it */
	in->len = (uint8_t)s->pos;
	return s->pos;
}

/* ---- formatting --------------------------------------------------------------- */

static const char *const r8n[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };
static const char *const r16n[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
static const char *const r32n[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
static const char *const sn[6] = { "es", "cs", "ss", "ds", "fs", "gs" };
static const char *const ean[9] = { "bx+si", "bx+di", "bp+si", "bp+di", "si", "di", "bp", "bx", "" };

static int fmt_op(const Operand *o, char *out, int n)
{
	switch (o->type) {
	case OT_REG:
		return snprintf(out, n, "%s", o->size == 1 ? r8n[o->reg] : o->size == 2 ? r16n[o->reg] : r32n[o->reg]);
	case OT_SREG:
		return snprintf(out, n, "%s", sn[o->reg]);
	case OT_IMM:
		return snprintf(out, n, "0x%x", o->imm);
	case OT_MEM: {
		const char *sz = o->size == 1 ? "byte " : o->size == 2 ? "word " : o->size == 4 ? "dword " :
		                 o->size == 6 ? "fword " : "";
		if (o->ea == EA_DISP)
			return snprintf(out, n, "%s%s:[0x%x]", sz, sn[o->seg], o->disp);
		if (o->disp)
			return snprintf(out, n, "%s%s:[%s%+d]", sz, sn[o->seg], ean[o->ea], (int16_t)o->disp);
		return snprintf(out, n, "%s%s:[%s]", sz, sn[o->seg], ean[o->ea]);
	}
	}
	return 0;
}

void x86_format(const Insn *in, uint16_t cs, uint16_t ip, char *out, int outsize)
{
	int n = 0;
	(void)cs;
	(void)ip;

	if (in->rep)
		n += snprintf(out + n, outsize - n, "%s ", in->rep == 0xF3 ? "rep" : "repne");
	switch (in->op) {
	case OP_JCC:
		n += snprintf(out + n, outsize - n, "j%s 0x%x", cc_names[in->cc], in->a.imm);
		return;
	case OP_SETCC:
		n += snprintf(out + n, outsize - n, "set%s ", cc_names[in->cc]);
		fmt_op(&in->a, out + n, outsize - n);
		return;
	case OP_JMPF: case OP_CALLF:
		snprintf(out + n, outsize - n, "%s 0x%x:0x%x", op_names[in->op], in->a.disp, in->a.imm);
		return;
	case OP_FPU:
		n += snprintf(out + n, outsize - n, "fpu %s%x/%02x ", (in->flags & IF_EMU) ? "emu " : "",
		              0xD8 + in->fpu_esc, in->modrm);
		if (in->a.type == OT_MEM)
			fmt_op(&in->a, out + n, outsize - n);
		return;
	case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS: case OP_INS: case OP_OUTS:
		snprintf(out + n, outsize - n, "%s%c%s", op_names[in->op], in->osz == 1 ? 'b' : in->osz == 2 ? 'w' : 'd',
		         in->seg != S_DS ? (in->seg == S_ES ? " es:" : in->seg == S_CS ? " cs:" : " ss:") : "");
		return;
	default:
		break;
	}
	n += snprintf(out + n, outsize - n, "%s", op_names[in->op]);
	if (in->a.type != OT_NONE) {
		n += snprintf(out + n, outsize - n, " ");
		n += fmt_op(&in->a, out + n, outsize - n);
	}
	if (in->b.type != OT_NONE) {
		n += snprintf(out + n, outsize - n, ", ");
		n += fmt_op(&in->b, out + n, outsize - n);
	}
	if (in->c.type != OT_NONE) {
		n += snprintf(out + n, outsize - n, ", ");
		fmt_op(&in->c, out + n, outsize - n);
	}
}
