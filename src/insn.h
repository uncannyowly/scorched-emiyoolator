/*
 * Decoded x86 instructions (real mode, 8086..286 plus the common 386 forms
 * with the 66h operand-size prefix, x87, and Borland's FPU-emulator
 * interrupts INT 34h..3Eh, which are decoded as the FPU instructions they
 * stand for).
 */
#ifndef INSN_H
#define INSN_H

#include <stdint.h>

enum {
	OT_NONE,
	OT_REG,  /* general register, number per size (8-bit: AL CL DL BL AH CH DH BH) */
	OT_SREG, /* segment register */
	OT_IMM,
	OT_MEM,
};

/* 16-bit effective address forms (modrm rm field), plus a pure displacement */
enum { EA_BX_SI, EA_BX_DI, EA_BP_SI, EA_BP_DI, EA_SI, EA_DI, EA_BP, EA_BX, EA_DISP };

typedef struct {
	uint8_t type;
	uint8_t size; /* bytes: 1, 2, 4; memory also 6 (far pointer 16:32), 8, 10 */
	uint8_t reg;  /* OT_REG / OT_SREG */
	uint8_t seg;  /* OT_MEM: segment register used */
	uint8_t ea;   /* OT_MEM: EA_* */
	uint16_t disp;
	uint32_t imm;
} Operand;

enum {
	OP_BAD,
	/* ALU, two operands; a = destination */
	OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP, OP_TEST,
	OP_INC, OP_DEC, OP_NEG, OP_NOT,
	OP_MUL, OP_IMUL1, OP_IMUL, OP_DIV, OP_IDIV, /* IMUL: a = b * c (two/three operand forms) */
	OP_ROL, OP_ROR, OP_RCL, OP_RCR, OP_SHL, OP_SHR, OP_SAL, OP_SAR, /* a by b */
	OP_SHLD, OP_SHRD, /* a, b by c */
	OP_DAA, OP_DAS, OP_AAA, OP_AAS, OP_AAM, OP_AAD, OP_SALC,
	OP_BT, OP_BTS, OP_BTR, OP_BTC, OP_BSF, OP_BSR,
	/* data movement */
	OP_MOV, OP_MOVZX, OP_MOVSX, OP_XCHG, OP_LEA,
	OP_LDS, OP_LES, OP_LSS, OP_LFS, OP_LGS, /* a = reg, b = mem far pointer */
	OP_PUSH, OP_POP, OP_PUSHA, OP_POPA, OP_PUSHF, OP_POPF, OP_LAHF, OP_SAHF,
	OP_CBW, OP_CWD, OP_XLAT, OP_SETCC,
	OP_IN, OP_OUT, /* a = AL/AX, b = port (imm or DX register) */
	OP_ENTER, OP_LEAVE, OP_BOUND,
	OP_CLC, OP_STC, OP_CMC, OP_CLD, OP_STD, OP_CLI, OP_STI,
	OP_NOP, OP_WAIT, OP_SMSW,
	/* strings: osz = element size, rep = 0 / 0xF3 / 0xF2, seg = source segment */
	OP_MOVS, OP_CMPS, OP_STOS, OP_LODS, OP_SCAS, OP_INS, OP_OUTS,
	/* control flow */
	OP_JMP,     /* a.imm = target offset */
	OP_JCC,     /* cc, a.imm = target */
	OP_JCXZ, OP_LOOP, OP_LOOPE, OP_LOOPNE,
	OP_JMPF,    /* a.imm = offset, a.disp = segment */
	OP_JMP_IND, /* a = r/m16 */
	OP_JMPF_IND, /* a = m16:16 */
	OP_CALL, OP_CALLF, OP_CALL_IND, OP_CALLF_IND,
	OP_RET, OP_RETF, OP_IRET, /* a.imm = bytes to pop (RET n) */
	OP_INT,     /* a.imm = vector */
	OP_INTO, OP_HLT,
	/* x87: fpu_esc 0..7 (D8..DF), modrm; a = memory operand if mod != 3 */
	OP_FPU,
	OP_FPU_SHORTCUT, /* Borland INT 3Eh xx 90: a.imm = xx */
	OP_COUNT
};

/* Insn.flags */
#define IF_EMU 0x01  /* x87 instruction written as a Borland emulator INT */
#define IF_LOCK 0x02
#define IF_386 0x04  /* needs a 386 (66h prefix or 0F opcode) */

typedef struct {
	uint16_t op;
	uint8_t len;
	uint8_t osz;     /* operand size in bytes */
	uint8_t rep;     /* 0, 0xF3, 0xF2 */
	uint8_t seg;     /* segment override or default data segment (S_DS) */
	uint8_t cc;      /* condition code for JCC/SETCC */
	uint8_t flags;
	uint8_t fpu_esc; /* OP_FPU: 0..7 */
	uint8_t modrm;   /* OP_FPU: the modrm byte */
	uint8_t fmask;   /* status flags to compute (FM_*): all, unless the
	                    recompiler proved some are overwritten before use */
	Operand a, b, c;
} Insn;

#define FM_CF 0x01
#define FM_PF 0x02
#define FM_AF 0x04
#define FM_ZF 0x08
#define FM_SF 0x10
#define FM_OF 0x20
#define FM_ALL 0x3F

/*
 * Decodes the instruction at `code` (up to `avail` bytes) located at offset
 * `ip` of its code segment. Returns the length, or 0 if the bytes are not a
 * valid instruction (in->op = OP_BAD).
 */
int x86_decode(const uint8_t *code, int avail, uint16_t ip, Insn *in);

/* text form for logs and the generated code's comments */
void x86_format(const Insn *in, uint16_t cs, uint16_t ip, char *out, int outsize);

extern const char *const op_names[OP_COUNT];

#endif
