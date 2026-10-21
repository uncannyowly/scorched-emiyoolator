/*
 * Keyboard: the 8042 controller (ports 60h/64h) feeding IRQ 1 one byte at a
 * time, and the BIOS side (INT 9 translation into the BDA buffer, read by
 * INT 16h). Keys arrive as XT scan codes, 0xE0xx for extended ones.
 */
#include "machine.h"
#include "ops.h"

#define QSIZE 256

static uint8_t q[QSIZE];
static unsigned qh, qt;
static uint8_t data, obf;
static uint32_t events;

int pic_requested(int irq);
int pic_in_service(int irq);

static void feed(void)
{
	if (obf || qh == qt || pic_requested(1) || pic_in_service(1))
		return;
	data = q[qh];
	qh = (qh + 1) % QSIZE;
	obf = 1;
	pic_raise(1);
}

static void put(uint8_t b)
{
	unsigned n = (qt + 1) % QSIZE;
	if (n == qh)
		return;
	q[qt] = b;
	qt = n;
}

void kbd_key(int code, int down)
{
	events++;
	if (code & 0xE000)
		put(0xE0);
	put((uint8_t)((code & 0x7F) | (down ? 0 : 0x80)));
	feed();
}

void kbd_eoi(void)
{
	feed();
}

void kbd_poll(void)
{
	feed();
}

/* bytes waiting in the controller (the BIOS buffer is the program's business:
 * a game may leave keys there while it waits for something else) */
int kbd_pending(void)
{
	return qh != qt || obf;
}

/* counts key presses and releases, for the idle check */
uint32_t kbd_events(void)
{
	return events;
}

static uint8_t kbd_in(uint16_t port)
{
	if (port == 0x60) {
		obf = 0;
		return data;
	}
	/* 64h: status. bit 0 output full, bit 2 self test passed, bit 4 unlocked */
	return (uint8_t)(0x14 | obf);
}

static void kbd_out(uint16_t port, uint8_t v)
{
	static int expect_data;
	if (port == 0x60) {
		/* commands to the keyboard (LEDs, typematic rate): acknowledge */
		if (expect_data) {
			expect_data = 0;
		} else if (v == 0xED || v == 0xF3) {
			expect_data = 1;
		}
		put(0xFA);
		feed();
	}
	/* 64h controller commands (A20, enable/disable) are accepted silently */
}

/* ---- BIOS translation ------------------------------------------------------------------- */

static const char norm[0x3A] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char shifted[0x3A] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

#define SH_RSHIFT 0x01
#define SH_LSHIFT 0x02
#define SH_CTRL 0x04
#define SH_ALT 0x08

static void buffer_put(uint16_t key)
{
	uint16_t head = rd16(0x41A), tail = rd16(0x41C), next = (uint16_t)(tail + 2);
	if (next >= rd16(0x482))
		next = rd16(0x480);
	if (next == head)
		return; /* full: the real BIOS beeps */
	wr16(LIN(0x40, tail), key);
	wr16(0x41C, next);
}

uint8_t kbd_shift_flags(void)
{
	return rd8(0x417);
}

void kbd_bios_int9(void)
{
	uint8_t sc = (uint8_t)port_in(0x60, 1);
	uint8_t flags = rd8(0x417), e0 = rd8(0x496) & 2;
	uint8_t make = sc & 0x7F, up = sc & 0x80;
	uint16_t key = 0;

	if (sc == 0xE0 || sc == 0xE1) {
		wr8(0x496, (uint8_t)(rd8(0x496) | 2));
		return;
	}
	wr8(0x496, (uint8_t)(rd8(0x496) & ~2));
	if (sc == 0xFA || sc == 0xFE)
		return;
	switch (make) {
	case 0x2A: if (!e0) flags = up ? (flags & ~SH_LSHIFT) : (flags | SH_LSHIFT); goto store;
	case 0x36: flags = up ? (flags & ~SH_RSHIFT) : (flags | SH_RSHIFT); goto store;
	case 0x1D: flags = up ? (flags & ~SH_CTRL) : (flags | SH_CTRL); goto store;
	case 0x38: flags = up ? (flags & ~SH_ALT) : (flags | SH_ALT); goto store;
	case 0x3A: if (!up) flags ^= 0x40; goto store;
	case 0x45: if (!up) flags ^= 0x20; goto store;
	case 0x46: if (!up) flags ^= 0x10; goto store;
	default: break;
	}
	if (up)
		return;
	{
		int shift = (flags & (SH_LSHIFT | SH_RSHIFT)) != 0, ctrl = (flags & SH_CTRL) != 0;
		int alt = (flags & SH_ALT) != 0, caps = (flags & 0x40) != 0;
		if (make >= 0x3B && make <= 0x44) { /* F1-F10 */
			int n = make - 0x3B;
			key = (uint16_t)((alt ? 0x68 + n : ctrl ? 0x5E + n : shift ? 0x54 + n : make) << 8);
		} else if (make == 0x57 || make == 0x58) { /* F11 F12 */
			int n = make - 0x57;
			key = (uint16_t)((alt ? 0x8B + n : ctrl ? 0x89 + n : shift ? 0x87 + n : 0x85 + n) << 8);
		} else if (make >= 0x47 && make <= 0x53) { /* keypad / cursor keys */
			if (make == 0x4A)
				key = 0x4A2D;
			else if (make == 0x4E)
				key = 0x4E2B;
			else if (!e0 && ((flags & 0x20) != 0) != shift && make != 0x4C) {
				static const char kp[] = "789-456+1230.";
				key = (uint16_t)((make << 8) | (uint8_t)kp[make - 0x47]);
			} else {
				key = (uint16_t)((make << 8) | (e0 ? 0xE0 : 0));
				if (ctrl) {
					static const uint8_t ck[13] = { 0x77, 0x8D, 0x84, 0x8E, 0x73, 0x8F, 0x74,
					                                0x90, 0x75, 0x91, 0x76, 0x92, 0x93 };
					key = (uint16_t)(ck[make - 0x47] << 8);
				}
			}
		} else if (make < 0x3A) {
			char c = shift ? shifted[make] : norm[make];
			if (caps && c >= 'a' && c <= 'z' && !shift)
				c = (char)(c - 32);
			else if (caps && c >= 'A' && c <= 'Z' && shift)
				c = (char)(c + 32);
			if (alt) {
				key = (uint16_t)(make << 8);
				if (make >= 0x02 && make <= 0x0D)
					key = (uint16_t)((make + 0x76) << 8);
			} else if (ctrl) {
				char l = norm[make];
				if (l >= 'a' && l <= 'z')
					key = (uint16_t)((make << 8) | (l - 'a' + 1));
				else if (make == 0x1C)
					key = 0x1C0A;
				else if (make == 0x0E)
					key = 0x0E7F;
				else
					key = 0;
			} else if (make == 0x0F && shift) {
				key = 0x0F00;
			} else if (make == 0x1C && e0) {
				key = 0xE00D;
			} else if (make == 0x35 && e0) {
				key = 0xE02F;
			} else {
				key = (uint16_t)((make << 8) | (uint8_t)c);
			}
		}
		if (key)
			buffer_put(key);
	}
	return;
store:
	wr8(0x417, flags);
}

int kbd_bios_read(int peek, int extended, uint16_t *key)
{
	uint16_t head = rd16(0x41A), tail = rd16(0x41C), k;

	if (head == tail)
		return 0;
	k = rd16(LIN(0x40, head));
	if (!extended && (k & 0xFF) == 0xE0 && (k >> 8))
		k &= 0xFF00;
	if (!extended && (k >> 8) == 0xE0)
		k = (uint16_t)(0x1C00 | (k & 0xFF));
	*key = k;
	if (!peek) {
		head = (uint16_t)(head + 2);
		if (head >= rd16(0x482))
			head = rd16(0x480);
		wr16(0x41A, head);
	}
	return 1;
}

void kbd_init(void)
{
	port_register(0x60, 0x60, kbd_in, kbd_out);
	port_register(0x64, 0x64, kbd_in, kbd_out);
}
