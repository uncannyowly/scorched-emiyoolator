/*
 * The interpreter: one instruction at a time, using the same semantics
 * (ops.h) as the recompiled code. It runs whatever the recompiler did not
 * find, and serves as the reference the compiled code is tested against.
 */
#include "machine.h"
#include "ops.h"


/* decoded-instruction cache, checked against the code bytes on every use */
#define DCACHE_BITS 14
typedef struct {
	uint32_t lin;
	uint8_t bytes[15];
	uint8_t valid;
	Insn in;
} DEntry;
static DEntry dcache[1u << DCACHE_BITS];

int interp_xfer_kind;

static const Insn *fetch(uint32_t lin, uint16_t ip)
{
	DEntry *e = &dcache[(lin ^ (lin >> DCACHE_BITS)) & ((1u << DCACHE_BITS) - 1)];

	if (e->valid && e->lin == lin && memcmp(e->bytes, mem + lin, e->in.len) == 0)
		return &e->in;
	x86_decode(mem + lin, 15, ip, &e->in);
	e->lin = lin;
	e->valid = 1;
	memcpy(e->bytes, mem + lin, 15);
	return &e->in;
}

static void bad_insn(uint16_t ip)
{
	uint32_t lin = LIN(sCS, ip);
	int i;
	char hex[64] = "";
	for (i = 0; i < 12; i++)
		snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x ", mem[lin + i]);
	LOG("invalid instruction at %04x:%04x: %s", sCS, ip, hex);
	machine_exit(3);
}

static void trace(const Insn *in, uint16_t ip)
{
	char txt[96];
	x86_format(in, sCS, ip, txt, sizeof(txt));
	LOG("%04x:%04x %-32s ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x ds=%04x es=%04x ss=%04x f=%03x",
	    sCS, ip, txt, rAX, rBX, rCX, rDX, rSI, rDI, rBP, rSP, sDS, sES, sSS, flags_get() & 0xFFF);
}

/* after a taken backward branch: the place where interrupts come in */
static inline void checkpoint(void)
{
	if (unlikely(cpu.icount >= cpu.deadline))
		machine_event();
}

int interp_step(void)
{
	uint16_t ip = cpu.ip;
	const Insn *in = fetch(LIN(sCS, ip), ip);
	uint16_t next = (uint16_t)(ip + in->len);

	if (unlikely(mcfg.trace) && (mcfg.trace == 1 || sCS == mcfg.trace))
		trace(in, ip);
	cpu.ip = next;
	switch (in->op) {
	case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND:
	case OP_SUB: case OP_XOR: case OP_CMP: case OP_TEST:
		exec_alu(in);
		break;
	case OP_INC: case OP_DEC: exec_incdec(in); break;
	case OP_NEG: exec_neg(in); break;
	case OP_NOT: exec_not(in); break;
	case OP_MUL: case OP_IMUL1: exec_mul(in); break;
	case OP_IMUL: exec_imul(in); break;
	case OP_DIV: case OP_IDIV:
		if (exec_div(in)) {
			cpu.icount++;
			cpu_interrupt(0, ip);
			return 1;
		}
		break;
	case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
	case OP_SHL: case OP_SHR: case OP_SAL: case OP_SAR:
		exec_shift(in);
		break;
	case OP_SHLD: case OP_SHRD: exec_shxd(in); break;
	case OP_DAA: bcd_daa(); break;
	case OP_DAS: bcd_das(); break;
	case OP_AAA: bcd_aaa(); break;
	case OP_AAS: bcd_aas(); break;
	case OP_AAM:
		if (exec_aam(in)) {
			cpu.icount++;
			cpu_interrupt(0, ip);
			return 1;
		}
		break;
	case OP_AAD: exec_aad(in); break;
	case OP_SALC: exec_salc(); break;
	case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: exec_bt(in); break;
	case OP_BSF: case OP_BSR: exec_bsx(in); break;
	case OP_MOV: exec_mov(in); break;
	case OP_MOVZX: case OP_MOVSX: exec_movx(in); break;
	case OP_XCHG: exec_xchg(in); break;
	case OP_LEA: exec_lea(in); break;
	case OP_LDS: case OP_LES: case OP_LSS: case OP_LFS: case OP_LGS: exec_lxs(in); break;
	case OP_PUSH: exec_push(in); break;
	case OP_POP: exec_pop(in); break;
	case OP_PUSHA: exec_pusha(in); break;
	case OP_POPA: exec_popa(in); break;
	case OP_PUSHF: exec_pushf(in); break;
	case OP_POPF: exec_popf(in); break;
	case OP_LAHF: exec_lahf(); break;
	case OP_SAHF: exec_sahf(); break;
	case OP_CBW: exec_cbw(in); break;
	case OP_CWD: exec_cwd(in); break;
	case OP_XLAT: exec_xlat(in); break;
	case OP_SETCC: exec_setcc(in); break;
	case OP_IN: exec_in(in); break;
	case OP_OUT: exec_out(in); break;
	case OP_ENTER: exec_enter(in); break;
	case OP_LEAVE: exec_leave(); break;
	case OP_BOUND:
		if (exec_bound(in)) {
			cpu.icount++;
			cpu_interrupt(5, ip);
			return 1;
		}
		break;
	case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD: case OP_CLI: case OP_STI:
		exec_flagop(in);
		break;
	case OP_NOP: case OP_WAIT: break;
	case OP_SMSW: exec_smsw(in); break;
	case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS: case OP_INS: case OP_OUTS:
		exec_string(in);
		break;
	case OP_FPU:
		if (fpu_exec(in)) {
			LOG("unsupported FPU instruction %02x %02x at %04x:%04x", 0xD8 + in->fpu_esc, in->modrm, sCS, ip);
			machine_exit(3);
		}
		break;
	case OP_FPU_SHORTCUT: fpu_shortcut((int)in->a.imm); break;

	/* ---- control flow ---- */
	case OP_JMP:
		interp_xfer_kind = 0;
		cpu.ip = (uint16_t)in->a.imm;
		cpu.icount++;
		if (cpu.ip <= ip)
			checkpoint();
		return 1;
	case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: {
		interp_xfer_kind = 0;
		int taken;
		switch (in->op) {
		case OP_JCC: taken = cond(in->cc); break;
		case OP_JCXZ: taken = rCX == 0; break;
		case OP_LOOP: taken = --rCX != 0; break;
		case OP_LOOPE: taken = --rCX != 0 && cpu.zf; break;
		default: taken = --rCX != 0 && !cpu.zf; break;
		}
		cpu.icount++;
		if (!taken)
			return 0;
		cpu.ip = (uint16_t)in->a.imm;
		if (cpu.ip <= ip)
			checkpoint();
		return 1;
	}
	case OP_JMPF:
		interp_xfer_kind = 1;
		sCS = in->a.disp;
		cpu.ip = (uint16_t)in->a.imm;
		cpu.icount++;
		return 1;
	case OP_JMP_IND:
		interp_xfer_kind = 1;
		cpu.ip = (uint16_t)opnd_read(&in->a, 2);
		cpu.icount++;
		return 1;
	case OP_JMPF_IND: {
		interp_xfer_kind = 1;
		uint32_t a = ea_linear(&in->a);
		cpu.ip = rd16(a);
		sCS = rd16(a + 2);
		cpu.icount++;
		return 1;
	}
	case OP_CALL:
		interp_xfer_kind = 1;
		push16(next);
		cpu.ip = (uint16_t)in->a.imm;
		cpu.icount++;
		return 1;
	case OP_CALLF:
		interp_xfer_kind = 1;
		push16(sCS);
		push16(next);
		sCS = in->a.disp;
		cpu.ip = (uint16_t)in->a.imm;
		cpu.icount++;
		return 1;
	case OP_CALL_IND: {
		interp_xfer_kind = 1;
		uint16_t t = (uint16_t)opnd_read(&in->a, 2);
		push16(next);
		cpu.ip = t;
		cpu.icount++;
		return 1;
	}
	case OP_CALLF_IND: {
		interp_xfer_kind = 1;
		uint32_t a = ea_linear(&in->a);
		uint16_t off = rd16(a), seg = rd16(a + 2);
		push16(sCS);
		push16(next);
		sCS = seg;
		cpu.ip = off;
		cpu.icount++;
		return 1;
	}
	case OP_RET:
		interp_xfer_kind = 2;
		cpu.ip = pop16();
		rSP += (uint16_t)in->a.imm;
		cpu.icount++;
		return 1;
	case OP_RETF:
		interp_xfer_kind = 2;
		cpu.ip = pop16();
		sCS = pop16();
		rSP += (uint16_t)in->a.imm;
		cpu.icount++;
		return 1;
	case OP_IRET:
		interp_xfer_kind = 2;
		cpu.ip = pop16();
		sCS = pop16();
		flags_set(pop16());
		cpu.icount++;
		return 1;
	case OP_INT:
		cpu.icount++;
		cpu_interrupt((int)in->a.imm, next);
		return 1;
	case OP_INTO:
		cpu.icount++;
		if (!cpu.of)
			return 0;
		cpu_interrupt(4, next);
		return 1;
	case OP_HLT:
		interp_xfer_kind = 0;
		cpu.icount++;
		cpu.halted = 1;
		return 1;
	default:
		cpu.ip = ip;
		bad_insn(ip);
		return 1;
	}
	cpu.icount++;
	return 0;
}
