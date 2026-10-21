/*
 * Run loops, interrupt delivery, time, ports and the 8259 PIC.
 *
 * Execution model: compiled functions (from the recompiler) run natively and
 * call each other directly; anything they cannot handle (an unknown call
 * target, an unexpected return address) makes them "bail": they store the
 * exact CS:IP to continue at, set cpu.bail and return, and the nearest run
 * loop continues in the interpreter. The interpreter calls compiled code
 * again whenever control reaches a compiled function's entry.
 *
 * Interrupts are delivered at taken backward branches only (and while
 * waiting), in both the interpreter and the compiled code, so with virtual
 * time both execute identically and can be compared instruction counts and
 * all.
 */
#include "machine.h"
#include "ops.h"

#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

MachineConfig mcfg = { .realtime = 1, .mips = 25.0 };
FILE *log_file;

void logf_(const char *fmt, ...)
{
	va_list ap;
	FILE *f = log_file ? log_file : stderr;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

/* ---- time ---------------------------------------------------------------------- */

static uint64_t host_t0;
static uint64_t time_offset_ns; /* virtual mode: time spent waiting */
uint64_t slept_us, sleeps, sleep_max_us; /* real time spent sleeping (statistics) */
uint64_t sleep_step_us;         /* real time: what a short sleep really takes here */

static uint64_t host_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* virtual time: whole instructions per microsecond (integer math keeps
 * runs identical on every machine) */
static uint64_t v_ipus = 25;

uint64_t time_ns(void)
{
	if (mcfg.realtime)
		return host_ns() - host_t0;
	return cpu.icount * 1000 / v_ipus + time_offset_ns;
}

/* virtual time: the first instruction count at which time_us() >= t_us */
static uint64_t icount_at(uint64_t t_us)
{
	uint64_t t_ns = t_us * 1000;
	if (t_ns <= time_offset_ns)
		return 0;
	return ((t_ns - time_offset_ns) * v_ipus + 999) / 1000;
}

uint64_t time_us(void)
{
	return time_ns() / 1000;
}

/* real time: sleep about d microseconds; the kernel may give far more (the
 * Mini's turns 200 us into about 9 ms). SCORCH_SLEEPQ=10000 imitates such a
 * kernel for testing: every sleep ends on a 10 ms grid, one step late. */
static void host_sleep(useconds_t d)
{
	static int q = -1;
	if (q < 0) {
		const char *e = getenv("SCORCH_SLEEPQ");
		q = e ? atoi(e) : 0;
	}
	if (q > 0) {
		uint64_t now = host_ns() / 1000;
		d = (useconds_t)((now + d + q - 1) / q * q - now + q);
	}
	usleep(d);
}

void time_sleep_until_us(uint64_t t)
{
	if (mcfg.realtime) {
		uint64_t now = time_us();
		if (t > now) {
			useconds_t d = (useconds_t)(t - now > 20000 ? 20000 : t - now);
			uint64_t after;
			host_sleep(d);
			/* count what the kernel gave, and owe the game every timer tick of it */
			after = time_us();
			slept_us += after - now;
			if (after - now > sleep_max_us)
				sleep_max_us = after - now;
			sleeps++;
			pit_update_slept(after);
		}
	} else {
		uint64_t now = time_us();
		if (t > now)
			time_offset_ns += (t - now) * 1000;
	}
	/* time moved: whatever fell due is handled at the next checkpoint */
	cpu.deadline = cpu.icount;
}

/* ---- PIC ------------------------------------------------------------------------- */

static struct {
	uint8_t irr, isr, imr;
	uint8_t icw_step, read_isr;
} pic = { .imr = 0xB8 }; /* timer, keyboard, cascade and the FPU free on the master */

void pic_raise(int irq)
{
	pic.irr |= (uint8_t)(1 << irq);
	/* look at it at the next checkpoint, not at the next timed event: that one
	 * may be a frame, and runs must not depend on the frame rate (a keyboard
	 * byte fed at the end of the previous one's handler showed it) */
	cpu.deadline = cpu.icount;
}

void pic_lower(int irq)
{
	pic.irr &= (uint8_t)~(1 << irq);
}

int pic_requested(int irq)
{
	return (pic.irr >> irq) & 1;
}

int pic_in_service(int irq)
{
	return (pic.isr >> irq) & 1;
}

static int pic_best(void)
{
	uint8_t req = pic.irr & ~pic.imr;
	int i;
	for (i = 0; i < 8; i++) {
		if (pic.isr & (1 << i))
			return -1; /* a higher-priority interrupt is in service */
		if (req & (1 << i))
			return i;
	}
	return -1;
}

int pic_pending(void)
{
	return pic_best() >= 0;
}

/* something waits to be delivered as soon as interrupts allow */
int irq_waiting(void)
{
	int mouse_waiting(void);
	return (pic.irr & ~pic.imr) != 0 || mouse_waiting();
}

static int pic_ack(void)
{
	int i = pic_best();
	if (i < 0)
		return -1;
	pic.irr &= (uint8_t)~(1 << i);
	pic.isr |= (uint8_t)(1 << i);
	return 8 + i;
}

static uint8_t pic_in(uint16_t port)
{
	if (port == 0x21)
		return pic.imr;
	if (port == 0x20)
		return pic.read_isr ? pic.isr : pic.irr;
	return 0xFF; /* no slave PIC */
}

void kbd_eoi(void);

static void pic_out(uint16_t port, uint8_t v)
{
	if (port == 0x20) {
		if (v & 0x10) { /* ICW1: reinitialisation, keep the BIOS setup */
			pic.icw_step = 1;
			return;
		}
		if ((v & 0x18) == 0x08) { /* OCW3 */
			if (v & 2)
				pic.read_isr = v & 1;
			return;
		}
		if ((v & 0xE0) == 0x20) { /* non-specific EOI */
			int i;
			for (i = 0; i < 8; i++) {
				if (pic.isr & (1 << i)) {
					pic.isr &= (uint8_t)~(1 << i);
					if (i == 1)
						kbd_eoi();
					break;
				}
			}
		} else if ((v & 0xE0) == 0x60) { /* specific EOI */
			pic.isr &= (uint8_t)~(1 << (v & 7));
			if ((v & 7) == 1)
				kbd_eoi();
		}
		return;
	}
	if (port == 0x21) {
		if (pic.icw_step) {
			/* ICW2..4: ignore (the vector base stays 08h) */
			if (++pic.icw_step > 3)
				pic.icw_step = 0;
			return;
		}
		pic.imr = v;
	}
}

/* ---- ports ------------------------------------------------------------------------- */

#define PORTS 0x400
static PortIn port_in_tab[PORTS];
static PortOut port_out_tab[PORTS];

void port_register(uint16_t first, uint16_t last, PortIn in, PortOut out)
{
	unsigned p;
	for (p = first; p <= last && p < PORTS; p++) {
		port_in_tab[p] = in;
		port_out_tab[p] = out;
	}
}

static uint8_t port_in8(uint16_t port)
{
	if (port < PORTS && port_in_tab[port])
		return port_in_tab[port](port);
	if (mcfg.log_io)
		LOG("in  %04x (unhandled) at %04x:%04x", port, sCS, cpu.ip);
	return 0xFF;
}

static void port_out8(uint16_t port, uint8_t v)
{
	if (port < PORTS && port_out_tab[port]) {
		port_out_tab[port](port, v);
		return;
	}
	if (mcfg.log_io)
		LOG("out %04x,%02x (unhandled) at %04x:%04x", port, v, sCS, cpu.ip);
}

uint32_t port_in(uint16_t port, int size)
{
	uint32_t v = port_in8(port);
	if (size >= 2)
		v |= (uint32_t)port_in8((uint16_t)(port + 1)) << 8;
	if (size == 4)
		v |= ((uint32_t)port_in8((uint16_t)(port + 2)) << 16) | ((uint32_t)port_in8((uint16_t)(port + 3)) << 24);
	return v;
}

void port_out(uint16_t port, uint32_t v, int size)
{
	port_out8(port, (uint8_t)v);
	if (size >= 2)
		port_out8((uint16_t)(port + 1), (uint8_t)(v >> 8));
	if (size == 4) {
		port_out8((uint16_t)(port + 2), (uint8_t)(v >> 16));
		port_out8((uint16_t)(port + 3), (uint8_t)(v >> 24));
	}
}

/* ---- events ------------------------------------------------------------------------ */

#define POLL_US 4000u     /* input */
#define RT_QUANTUM 4000u  /* real time: most instructions between host clock reads */

static uint64_t next_frame_us, next_poll_us;
static uint32_t frame_us = 16667;

/* real time: instructions per microsecond, measured, to aim the next check
 * at the moment the next timer interrupt falls due */
static double rt_ipus = 10.0;
static uint64_t rt_icount0, rt_t0;

static void schedule(uint64_t now)
{
	uint64_t t = next_frame_us, p = pit_next_irq_us();
	double insns;

	if (next_poll_us < t)
		t = next_poll_us;
	if (p < t)
		t = p;
	if (irq_waiting()) {
		/* masked for now (CLI, or one in service): try at the next checkpoint */
		cpu.deadline = cpu.icount + (mcfg.realtime ? 64 : 1);
		return;
	}
	if (mcfg.realtime) {
		if (now - rt_t0 >= 50000) {
			double ipus = (double)(cpu.icount - rt_icount0) / (double)(now - rt_t0);
			if (ipus > 0.5)
				rt_ipus = ipus;
			rt_t0 = now;
			rt_icount0 = cpu.icount;
		}
		insns = t > now ? (double)(t - now) * rt_ipus : 0.0;
		if (insns < 64)
			insns = 64;
		if (insns > RT_QUANTUM)
			insns = RT_QUANTUM;
		cpu.deadline = cpu.icount + (uint64_t)insns;
	} else {
		uint64_t d = icount_at(t);
		cpu.deadline = d > cpu.icount ? d : cpu.icount + 1;
	}
}

int mouse_deliver(void);
void kbd_poll(void);
uint64_t irq0_count; /* statistics */
FILE *event_trace;
static void trace_event(void);

static void deliver_irq(void)
{
	int vec;
	if (!cpu.ifl)
		return;
	vec = pic_ack();
	if (vec == 8)
		irq0_count++;
	if (vec < 0) {
		int mouse_waiting(void);
		if (mouse_waiting()) {
			if (event_trace)
				trace_event();
			if (mouse_deliver())
				cpu.halted = 0;
		}
		return;
	}
	if (event_trace)
		trace_event();
	cpu.halted = 0;
	cpu_interrupt(vec, cpu.ip);
}

static void check_exit(uint64_t now)
{
	if (mcfg.max_ms && now / 1000 >= mcfg.max_ms) {
		LOG("time limit reached (%llu ms)", (unsigned long long)mcfg.max_ms);
		machine_exit(0);
	}
}

static void trace_event(void)
{
	/* state at this interrupt point, for comparing interpreter and compiled runs */
	uint32_t h = 2166136261u, a, ds = LIN(LOAD_SEG + 0x4F38, 0);
	for (a = ds; a < ds + 0x10000; a += 4)
		h = (h ^ (mem[a] | (mem[a + 1] << 8) | (mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24))) * 16777619u;
	fprintf(event_trace, "%llu %04x:%04x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x "
	        "ds=%04x es=%04x ss=%04x f=%03x dgroup=%08x\n",
	        (unsigned long long)cpu.icount, sCS, cpu.ip, rAX, rBX, rCX, rDX, rSI, rDI, rBP, rSP, sDS, sES, sSS,
	        flags_get() & 0x700, h); /* status flags may differ where they are dead */
}

void machine_event(void)
{
	uint64_t now = time_us();

	pit_update(now);
	if (now >= next_poll_us) {
		frontend_poll();
		next_poll_us += POLL_US;
		if (next_poll_us <= now)
			next_poll_us = now + POLL_US;
		check_exit(now);
	}
	kbd_poll();
	if (now >= next_frame_us) {
		frontend_frame();
		next_frame_us += frame_us;
		if (next_frame_us < now)
			next_frame_us = now + frame_us;
	}
	deliver_irq();
	schedule(now);
}

/* ---- interrupts ------------------------------------------------------------------- */

void cpu_interrupt(int n, uint16_t ret_ip)
{
	interp_xfer_kind = 1;
	push16(flags_get());
	push16(sCS);
	push16(ret_ip);
	cpu.ifl = 0;
	cpu.tf = 0;
	cpu.ip = rd16((uint32_t)n * 4);
	sCS = rd16((uint32_t)n * 4 + 2);
}

/* ---- run loops ----------------------------------------------------------------------- */

static void wait_for_interrupt(void)
{
	/* HLT: let time pass until an interrupt is delivered */
	while (cpu.halted) {
		uint64_t now = time_us(), t = pit_next_irq_us();
		if (t > next_poll_us)
			t = next_poll_us;
		if (t > now + 20000)
			t = now + 20000;
		time_sleep_until_us(t);
		machine_event();
		if (!cpu.ifl && cpu.halted) {
			LOG("HLT with interrupts disabled at %04x:%04x", sCS, cpu.ip);
			machine_exit(1);
		}
	}
}

static long long interp_count;

/* ---- interpreted entry points (for the next recompile) ------------------------------------ */

#define UNC_BITS 14
static uint32_t unc_key[1 << UNC_BITS];
static uint32_t unc_hits[1 << UNC_BITS];
static int unc_count;

void uncompiled_note(uint16_t cs, uint16_t ip)
{
	uint32_t k = ((uint32_t)cs << 16) | ip, h = (k * 0x9E3779B1u) >> (32 - UNC_BITS);
	if (cs == BIOS_SEG)
		return;
	while (unc_key[h] && unc_key[h] != k)
		h = (h + 1) & ((1u << UNC_BITS) - 1);
	if (!unc_key[h]) {
		if (unc_count >= (1 << UNC_BITS) - 1)
			return;
		unc_key[h] = k;
		unc_count++;
	}
	unc_hits[h]++;
}

void uncompiled_write(const char *path)
{
	FILE *f;
	unsigned i;
	if (!unc_count)
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "# entry points the interpreter ran (CS:IP hits); tools/recomp.c reads these\n");
	for (i = 0; i < (1u << UNC_BITS); i++)
		if (unc_key[i])
			fprintf(f, "%04x:%04x %u\n", unc_key[i] >> 16, unc_key[i] & 0xFFFF, unc_hits[i]);
	fclose(f);
	LOG("%d interpreted entry points written to %s", unc_count, path);
}

void run_until(uint16_t cs, uint16_t ip, uint16_t sp_min)
{
	uint16_t ss = sSS;
	int note_returns = compiled_image_sum != 0 && !mcfg.no_compiled;

	for (;;) {
		CompiledFn f;

		if (cpu.ip == ip && sCS == cs && sSS == ss && rSP >= sp_min)
			return;
		if (cpu.halted) {
			wait_for_interrupt();
			continue;
		}
		if (sCS == BIOS_SEG) {
			bios_dispatch();
			continue;
		}
		f = mcfg.no_compiled ? NULL : compiled_lookup(sCS, cpu.ip);
		if (f) {
			f();
			cpu.bail = 0;
			continue;
		}
		if (interp_xfer_kind == 1 || (interp_xfer_kind == 2 && note_returns))
			uncompiled_note(sCS, cpu.ip);
		/* interpret up to the next control transfer, then look again */
		while (!interp_step()) {
			interp_count++;
			if (cpu.ip == ip && sCS == cs && sSS == ss && rSP >= sp_min)
				return;
		}
		interp_count++;
	}
}

void machine_run(void)
{
	host_t0 = host_ns();
	if (mcfg.fps > 0)
		frame_us = 1000000u / (uint32_t)mcfg.fps;
	v_ipus = mcfg.mips >= 1.0 ? (uint64_t)(mcfg.mips + 0.5) : 1;
	next_frame_us = frame_us;
	next_poll_us = 0;
	schedule(0);
	/* never returns: the program ends through INT 21h, AH=4Ch */
	run_until(0xFFFF, 0xFFFF, 0xFFFF);
}

/* ---- helpers for the compiled code ------------------------------------------------------ */

void compiled_int(int n, uint16_t ret_ip)
{
	uint16_t cs = sCS, sp = rSP;
	cpu_interrupt(n, ret_ip);
	run_until(cs, ret_ip, sp);
}

/* the caller has pushed its return address; returns once the callee pops it */
void compiled_call_far(uint16_t cs, uint16_t ip, uint16_t ret_cs, uint16_t ret_ip)
{
	uint16_t sp = rSP;
	sCS = cs;
	cpu.ip = ip;
	run_until(ret_cs, ret_ip, sp);
}

/* HLT in compiled code: wait for an interrupt, run it, continue after the HLT */
void compiled_hlt(uint16_t next_ip)
{
	uint16_t cs = sCS, sp = rSP;
	cpu.ip = next_ip;
	cpu.halted = 1;
	wait_for_interrupt();
	if (sCS != cs || cpu.ip != next_ip)
		run_until(cs, next_ip, sp);
}

void compiled_checkpoint(uint16_t resume_ip)
{
	uint16_t cs = sCS, sp = rSP;
	cpu.ip = resume_ip;
	machine_event();
	if (sCS != cs || cpu.ip != resume_ip)
		run_until(cs, resume_ip, sp);
}

long long machine_interp_count(void)
{
	return interp_count;
}

void machine_init(void)
{
	memset(mem, 0, sizeof(mem));
	memset(&cpu, 0, sizeof(cpu));
	port_register(0x20, 0x21, pic_in, pic_out);
	port_register(0xA0, 0xA1, NULL, NULL);
	bios_init();
	pit_init();
	kbd_init();
	vga_init();
	mouse_init();
	if (mcfg.realtime) {
		/* how long a short sleep really takes on this kernel: the idle code
		 * plans its sleeps with it */
		uint64_t t0 = host_ns();
		int i;
		for (i = 0; i < 5; i++)
			host_sleep(200);
		sleep_step_us = (host_ns() - t0) / 5000;
		LOG("timer: a 200 us sleep takes %llu us", (unsigned long long)sleep_step_us);
	}
}
