/*
 * BIOS: interrupt vectors, the BIOS data area, the fake ROMs and the HLE
 * entry points in segment F000.
 *
 * Vector n points to F000:n. When execution reaches CS = F000 the run loop
 * calls bios_dispatch(), which performs the service in C and returns with
 * IRET (the caller's FLAGS image on the stack carries CF/ZF results).
 * IP values from 0x100 up are continuations for services that call back
 * into the program (INT 8 -> INT 1Ch) and the VESA window function.
 */
#include "machine.h"
#include "ops.h"

#include <stdlib.h>
#include <time.h>

#define CONT_INT8_AFTER_1C 0x100
#define VESA_WINFUNC 0x180
#define MOUSE_CB_RET 0x190

/* VGA ROM fonts, from build/fonts.bin via the generated vgafont.c */
extern const uint8_t vga_font_8x8[2048], vga_font_8x14[3584], vga_font_8x16[4096];

#define FONT8_OFF 0x1000
#define FONT14_OFF 0x1800
#define FONT16_OFF 0x2600

static void iret(void)
{
	cpu.ip = pop16();
	sCS = pop16();
	flags_set(pop16());
}

static void retf(void)
{
	cpu.ip = pop16();
	sCS = pop16();
}

static void frame_flags(uint16_t set, uint16_t clear)
{
	uint32_t a = LIN(sSS, rSP + 4);
	wr16(a, (uint16_t)((rd16(a) | set) & ~clear));
}

void bios_init(void)
{
	uint32_t a;
	int n;
	time_t t = time(NULL);
	struct tm tm;
	static const char rom_id[] = "VGA/VBE BIOS (Scorched eMiYoolator)";

	/* vectors */
	for (n = 0; n < 256; n++) {
		wr16((uint32_t)n * 4, (uint16_t)n);
		wr16((uint32_t)n * 4 + 2, BIOS_SEG);
		mem[LIN(BIOS_SEG, n)] = 0xCF; /* IRET, for programs that peek at handlers */
	}
	mem[LIN(BIOS_SEG, 0x33)] = 0xFB; /* the mouse driver is present */
	/* data pointers */
	wr16(0x1F * 4, FONT8_OFF + 1024);
	wr16(0x1F * 4 + 2, VBIOS_SEG);
	wr16(0x43 * 4, FONT8_OFF);
	wr16(0x43 * 4 + 2, VBIOS_SEG);
	wr16(0x1D * 4, 0);
	wr16(0x1D * 4 + 2, 0);

	/* ROMs */
	memcpy(mem + LIN(VBIOS_SEG, FONT8_OFF), vga_font_8x8, sizeof(vga_font_8x8));
	memcpy(mem + LIN(VBIOS_SEG, FONT14_OFF), vga_font_8x14, sizeof(vga_font_8x14));
	memcpy(mem + LIN(VBIOS_SEG, FONT16_OFF), vga_font_8x16, sizeof(vga_font_8x16));
	mem[LIN(VBIOS_SEG, 0)] = 0x55;
	mem[LIN(VBIOS_SEG, 1)] = 0xAA;
	mem[LIN(VBIOS_SEG, 2)] = 0x40;
	memcpy(mem + LIN(VBIOS_SEG, 0x40), rom_id, sizeof(rom_id));
	memcpy(mem + LIN(BIOS_SEG, 0xFFF5), "06/04/95", 8);
	mem[LIN(BIOS_SEG, 0xFFFE)] = 0xFC; /* AT */
	mem[LIN(BIOS_SEG, 0x0F00)] = 0;    /* empty DBCS table */
	mem[LIN(BIOS_SEG, 0x0F01)] = 0;

	/* BIOS data area */
	a = 0x400;
	wr16(a + 0x10, 0x0022); /* equipment: x87, 80x25 colour */
	wr16(a + 0x13, 640);
	wr16(a + 0x1A, 0x1E);
	wr16(a + 0x1C, 0x1E);
	wr16(a + 0x80, 0x1E);
	wr16(a + 0x82, 0x3E);
	wr8(a + 0x49, 3);
	wr16(a + 0x4A, 80);
	wr16(a + 0x4C, 4096);
	wr16(a + 0x60, 0x0607);
	wr16(a + 0x63, 0x3D4);
	wr8(a + 0x65, 0x29);
	wr8(a + 0x66, 0x30);
	wr8(a + 0x84, 24);
	wr16(a + 0x85, 16);
	wr8(a + 0x87, 0x60);
	wr8(a + 0x88, 0x09);
	wr8(a + 0x89, 0x11);
	wr8(a + 0x8A, 0x0B);
	wr8(a + 0x96, 0x10);
	if (mcfg.realtime) {
		localtime_r(&t, &tm);
		wr32(a + 0x6C, (uint32_t)((tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) * 18.2065));
	} else {
		wr32(a + 0x6C, (uint32_t)(12 * 3600 * 18.2065));
	}
}

/* the timer tick of the BIOS (INT 8) */
void bios_tick(void)
{
	uint32_t t = rd32(0x46C) + 1;
	if (t >= 0x1800B0) {
		t = 0;
		wr8(0x470, 1);
	}
	wr32(0x46C, t);
}

/*
 * Waits for a key in the BIOS buffer (INT 16h, DOS console input). While
 * waiting, interrupts are taken so the program's timer and keyboard
 * handlers keep running. Returns 1 with the key, or 0 if `peek` and none.
 */
int bios_wait_key(int peek, int extended, uint16_t *key)
{
	for (;;) {
		uint16_t cs = sCS, ip = cpu.ip, sp = rSP;
		uint8_t ifl = cpu.ifl;

		if (kbd_bios_read(peek, extended, key))
			return 1;
		if (peek)
			return 0;
		cpu.ifl = 1;
		time_sleep_until_us(time_us() + 2000);
		machine_event();
		if (sCS != cs || cpu.ip != ip)
			run_until(cs, ip, sp);
		cpu.ifl = ifl;
	}
}

static void int16(void)
{
	uint16_t key;

	switch (rAH) {
	case 0x00: case 0x10:
		bios_wait_key(0, rAH == 0x10, &key);
		rAX = key;
		break;
	case 0x01: case 0x11:
		if (bios_wait_key(1, rAH == 0x11, &key)) {
			rAX = key;
			frame_flags(0, FLAG_ZF);
		} else {
			frame_flags(FLAG_ZF, 0);
		}
		break;
	case 0x02:
		rAL = kbd_shift_flags();
		break;
	case 0x12:
		rAL = kbd_shift_flags();
		rAH = 0;
		break;
	case 0x05: {
		/* store a key in the buffer */
		uint16_t head = rd16(0x41A), tail = rd16(0x41C), next = (uint16_t)(tail + 2);
		if (next >= rd16(0x482))
			next = rd16(0x480);
		if (next == head) {
			rAL = 1;
		} else {
			wr16(LIN(0x40, tail), rCX);
			wr16(0x41C, next);
			rAL = 0;
		}
		break;
	}
	default:
		break;
	}
}

static void int1a(void)
{
	time_t t;
	struct tm tm;
#define BCD(v) ((uint8_t)((((v) / 10) << 4) | ((v) % 10)))

	switch (rAH) {
	case 0x00:
		rCX = rd16(0x46E);
		rDX = rd16(0x46C);
		rAL = rd8(0x470);
		wr8(0x470, 0);
		break;
	case 0x01:
		wr16(0x46E, rCX);
		wr16(0x46C, rDX);
		break;
	case 0x02: case 0x04:
		t = mcfg.realtime ? time(NULL) : (time_t)(802267200 + time_us() / 1000000);
		if (mcfg.realtime)
			localtime_r(&t, &tm);
		else
			gmtime_r(&t, &tm);
		if (rAH == 0x02) {
			rCH = BCD(tm.tm_hour);
			rCL = BCD(tm.tm_min);
			rDH = BCD(tm.tm_sec);
			rDL = 0;
		} else {
			rCH = BCD((tm.tm_year + 1900) / 100);
			rCL = BCD(tm.tm_year % 100);
			rDH = BCD(tm.tm_mon + 1);
			rDL = BCD(tm.tm_mday);
		}
		frame_flags(0, FLAG_CF);
		break;
	default:
		frame_flags(FLAG_CF, 0);
		break;
	}
#undef BCD
}

static void int15(void)
{
	switch (rAH) {
	case 0x86: { /* wait CX:DX microseconds, taking interrupts */
		uint64_t end = time_us() + (((uint32_t)rCX << 16) | rDX);
		uint16_t cs = sCS, ip = cpu.ip, sp = rSP;
		uint8_t ifl = cpu.ifl;
		while (time_us() < end) {
			cpu.ifl = 1;
			time_sleep_until_us(end);
			machine_event();
			if (sCS != cs || cpu.ip != ip)
				run_until(cs, ip, sp);
			cpu.ifl = ifl;
		}
		frame_flags(0, FLAG_CF);
		break;
	}
	case 0x88:
		rAX = 0;
		frame_flags(0, FLAG_CF);
		break;
	case 0x4F: /* keyboard intercept: process the key */
		frame_flags(FLAG_CF, 0);
		break;
	case 0x90: case 0x91:
		rAH = 0;
		frame_flags(0, FLAG_CF);
		break;
	default:
		rAH = 0x86;
		frame_flags(FLAG_CF, 0);
		break;
	}
}

static void int2f(void)
{
	switch (rAX) {
	case 0x1600: rAL = 0; break;        /* no Windows */
	case 0x1680: rAL = 0x80; break;     /* no time slice release */
	case 0x4300: rAL = 0; break;        /* no XMS */
	case 0x1687: rAX = 1; break;        /* no DPMI */
	default: break;
	}
}

void bios_dispatch(void)
{
	uint16_t ip = cpu.ip;

	if (ip < 0x100) {
		switch (ip) {
		case 0x08: { /* timer */
			uint16_t v_off = rd16(0x1C * 4), v_seg = rd16(0x1C * 4 + 2);
			bios_tick();
			if (v_seg == BIOS_SEG && v_off == 0x1C) {
				port_out(0x20, 0x20, 1);
				iret();
			} else {
				/* call the program's INT 1Ch, then finish at the continuation */
				cpu.ip = CONT_INT8_AFTER_1C;
				cpu_interrupt(0x1C, CONT_INT8_AFTER_1C);
			}
			return;
		}
		case 0x09:
			kbd_bios_int9();
			port_out(0x20, 0x20, 1);
			iret();
			return;
		case 0x10: vga_int10(); break;
		case 0x11: rAX = rd16(0x410); break;
		case 0x12: rAX = rd16(0x413); break;
		case 0x15: int15(); break;
		case 0x16: int16(); break;
		case 0x1A: int1a(); break;
		case 0x20:
			LOG("dos: INT 20h exit");
			machine_exit(0);
		case 0x21: dos_int21(); break;
		case 0x24: rAL = 3; break; /* critical error: fail */
		case 0x2F: int2f(); break;
		case 0x33: mouse_int33(); break;
		case 0x00:
			LOG("divide error at %04x:%04x (no handler)", rd16(LIN(sSS, rSP + 2)), rd16(LIN(sSS, rSP)));
			machine_exit(3);
		case 0x05: case 0x06: case 0x07:
			LOG("exception %d at %04x:%04x (no handler)", ip, rd16(LIN(sSS, rSP + 2)), rd16(LIN(sSS, rSP)));
			machine_exit(3);
		case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
			port_out(0x20, 0x20, 1);
			break;
		case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
			port_out(0xA0, 0x20, 1);
			port_out(0x20, 0x20, 1);
			break;
		default:
			break;
		}
		iret();
		return;
	}
	switch (ip) {
	case CONT_INT8_AFTER_1C:
		port_out(0x20, 0x20, 1);
		iret();
		return;
	case VESA_WINFUNC:
		vesa_window_call();
		retf();
		return;
	case MOUSE_CB_RET:
		mouse_event_return();
		return;
	default:
		LOG("bios: unknown entry F000:%04x", ip);
		machine_exit(3);
	}
}

uint16_t bios_font_offset(int which)
{
	return which == 8 ? FONT8_OFF : which == 14 ? FONT14_OFF : FONT16_OFF;
}
