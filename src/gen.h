/*
 * Support for the recompiled game code (build/gen/seg_XXXX.c, written by
 * tools/recomp.c).
 *
 * Every x86 function becomes a C function that runs from its entry until
 * the x86 code returns (RET/RETF/IRET pop the return address into CS:IP and
 * the C function returns). Calls between compiled functions are C calls;
 * after each, the caller checks that execution came back to the expected
 * place. If not, or if the code meets something it cannot handle, it sets
 * cpu.bail and returns: CS:IP then says where to continue, and the run loop
 * goes on in the interpreter (see machine.c).
 */
#ifndef GEN_H
#define GEN_H

#include "cpu.h"
#include "insn.h"
#include "ops.h"

typedef void (*CompiledFn)(void);

CompiledFn compiled_lookup(uint16_t cs, uint16_t ip);
void compiled_int(int n, uint16_t ret_ip);
void compiled_call_far(uint16_t cs, uint16_t ip, uint16_t ret_cs, uint16_t ret_ip);
void compiled_checkpoint(uint16_t resume_ip);
void compiled_hlt(uint16_t next_ip);

/* taken backward branch: interrupts and host events come in here */
#define CHECK(ip_)                                                                \
	do {                                                                      \
		if (unlikely(cpu.icount >= cpu.deadline)) {                       \
			compiled_checkpoint(ip_);                                 \
			if (unlikely(cpu.bail))                                   \
				return;                                           \
		}                                                                 \
	} while (0)

/* hand over to the interpreter at ip_ */
#define BAIL(ip_)                                                                 \
	do {                                                                      \
		cpu.ip = (ip_);                                                   \
		cpu.bail = 1;                                                     \
		return;                                                           \
	} while (0)

/* after a call: did it come back here? */
#define RETURNED(cs_, ip_)                                                        \
	do {                                                                      \
		if (unlikely(cpu.bail | (cpu.ip != (ip_)) | (sCS != (cs_)))) {    \
			cpu.bail = 1;                                             \
			return;                                                   \
		}                                                                 \
	} while (0)

/* Borland's int86() writes "push bp; int nn; pop bp; retf" on the stack and
 * calls it: run that natively (counted exactly as the interpreter would) */
static inline int int86_stub(uint16_t cs, uint16_t ip)
{
	const uint8_t *b = mem + LIN(cs, ip);
	if (b[0] != 0x55 || b[1] != 0xCD || b[3] != 0x5D || b[4] != 0xCB)
		return 0;
	push16(rBP);
	cpu.icount += 2; /* push, int */
	compiled_int(b[2], (uint16_t)(ip + 3));
	rBP = pop16();
	cpu.ip = pop16();
	sCS = pop16();
	cpu.icount += 2; /* pop, retf */
	return 1;
}

/* call a compiled function or, if there is none, interpret until it returns */
static inline void call_any(uint16_t cs, uint16_t ip, uint16_t ret_cs, uint16_t ret_ip)
{
	CompiledFn f = compiled_lookup(cs, ip);
	sCS = cs;
	cpu.ip = ip;
	if (f)
		f();
	else if (!int86_stub(cs, ip))
		compiled_call_far(cs, ip, ret_cs, ret_ip);
}

/* jump (no return here): run compiled code there, or let the interpreter do it */
#define JUMP_ANY(cs_, ip_)                                                        \
	do {                                                                      \
		CompiledFn f_ = compiled_lookup(cs_, ip_);                        \
		sCS = (cs_);                                                      \
		cpu.ip = (ip_);                                                   \
		if (f_) {                                                         \
			f_();                                                     \
			return;                                                   \
		}                                                                 \
		cpu.bail = 1;                                                     \
		return;                                                           \
	} while (0)

#endif
