/*
 * Microsoft-compatible mouse driver (INT 33h).
 *
 * The frontend feeds motion (mickeys) or absolute positions and buttons.
 * A program-installed event handler (function 0Ch) is called the way the
 * driver's IRQ would call it: at the next interrupt point, with the
 * interrupted registers saved and restored around it.
 */
#include "machine.h"
#include "ops.h"

#include <stdlib.h>

#define MOUSE_CB_RET 0x190

static struct {
	int x, y;               /* driver coordinates */
	int minx, maxx, miny, maxy;
	int buttons;
	int show;               /* cursor shown when >= 0 */
	int mick_x, mick_y;     /* motion counters for function 0Bh */
	int raw_x, raw_y;       /* accumulated raw mickeys (for the handler's SI/DI) */
	int ratio_x, ratio_y;   /* mickeys per 8 pixels */
	int frac_x, frac_y;
	int press_count[3], release_count[3];
	int press_x[3], press_y[3], release_x[3], release_y[3];
	uint16_t handler_mask, handler_seg, handler_off;
	uint16_t pending;       /* event bits waiting for the handler (if one is installed) */
	uint32_t activity;      /* counts every change, for the idle check */
	int cb_active;
	Cpu saved;
	int page;
} m;

static void clamp(void)
{
	if (m.x < m.minx) m.x = m.minx;
	if (m.x > m.maxx) m.x = m.maxx;
	if (m.y < m.miny) m.y = m.miny;
	if (m.y > m.maxy) m.y = m.maxy;
}

static void reset(void)
{
	int w = vga_mode_width(), h = vga_mode_height();
	m.minx = 0;
	m.miny = 0;
	m.maxx = (w == 320 ? 640 : w) - 1;
	m.maxy = h - 1;
	m.x = (m.maxx + 1) / 2;
	m.y = (m.maxy + 1) / 2;
	m.show = -1;
	m.mick_x = m.mick_y = 0;
	m.ratio_x = 8;
	m.ratio_y = 16;
	m.handler_mask = 0;
	m.pending = 0;
	memset(m.press_count, 0, sizeof(m.press_count));
	memset(m.release_count, 0, sizeof(m.release_count));
}

void mouse_init(void)
{
	reset();
}

void mouse_move(int dx, int dy)
{
	if (!dx && !dy)
		return;
	{
		static int log_moves = -1;
		if (log_moves < 0)
			log_moves = getenv("SCORCH_MOUSELOG") != NULL;
		if (log_moves)
			LOG("t=%llu move %d,%d", (unsigned long long)(time_us() / 1000), dx, dy);
	}
	m.mick_x += dx;
	m.mick_y += dy;
	m.raw_x += dx;
	m.raw_y += dy;
	m.frac_x += dx * 8;
	m.frac_y += dy * 8;
	m.x += m.frac_x / m.ratio_x;
	m.y += m.frac_y / m.ratio_y;
	m.frac_x %= m.ratio_x;
	m.frac_y %= m.ratio_y;
	clamp();
	m.activity++;
	if (m.handler_mask)
		m.pending |= 1;
}

/* absolute position in screen pixels of the current mode */
void mouse_set_pos(int px, int py)
{
	int w = vga_mode_width(), h = vga_mode_height();
	int nx, ny, dx, dy;
	if (w <= 0 || h <= 0)
		return;
	nx = m.minx + px * (m.maxx - m.minx + 1) / w;
	ny = m.miny + py * (m.maxy - m.miny + 1) / h;
	dx = nx - m.x;
	dy = ny - m.y;
	if (!dx && !dy)
		return;
	m.mick_x += dx * m.ratio_x / 8;
	m.mick_y += dy * m.ratio_y / 8;
	m.raw_x += dx * m.ratio_x / 8;
	m.raw_y += dy * m.ratio_y / 8;
	m.x = nx;
	m.y = ny;
	clamp();
	m.activity++;
	if (m.handler_mask)
		m.pending |= 1;
}

void mouse_buttons(int mask)
{
	int b;
	for (b = 0; b < 3; b++) {
		int was = (m.buttons >> b) & 1, now = (mask >> b) & 1;
		static const int pbit[3] = { 0x02, 0x08, 0x20 }, rbit[3] = { 0x04, 0x10, 0x40 };
		if (now && !was) {
			m.press_count[b]++;
			m.press_x[b] = m.x;
			m.press_y[b] = m.y;
			if (m.handler_mask)
				m.pending |= (uint16_t)pbit[b];
			m.activity++;
		} else if (!now && was) {
			m.release_count[b]++;
			m.release_x[b] = m.x;
			m.release_y[b] = m.y;
			if (m.handler_mask)
				m.pending |= (uint16_t)rbit[b];
			m.activity++;
		}
	}
	m.buttons = mask & 7;
}

void mouse_get_screen_pos(int *x, int *y, int *visible)
{
	int w = vga_mode_width(), h = vga_mode_height();
	int rx = m.maxx - m.minx + 1, ry = m.maxy - m.miny + 1;
	*x = rx > 0 ? (m.x - m.minx) * w / rx : 0;
	*y = ry > 0 ? (m.y - m.miny) * h / ry : 0;
	*visible = m.show >= 0;
}

/* ---- the program's event handler ------------------------------------------------- */

/* an event the program's handler has not seen yet */
int mouse_waiting(void)
{
	return !m.cb_active && (m.pending & m.handler_mask) && (m.handler_seg || m.handler_off);
}

/* called at an interrupt point; returns 1 if the handler was entered */
int mouse_deliver(void)
{
	uint16_t ev;
	if (m.cb_active || !m.pending || !(m.pending & m.handler_mask) || !cpu.ifl ||
	    (m.handler_seg == 0 && m.handler_off == 0))
		return 0;
	ev = m.pending & m.handler_mask;
	m.pending = 0;
	m.saved = cpu;
	m.cb_active = 1;
	/* an interrupt frame back to the program, then a far call frame back to us */
	push16(flags_get());
	push16(sCS);
	push16(cpu.ip);
	push16(BIOS_SEG);
	push16(MOUSE_CB_RET);
	cpu.ifl = 0;
	rAX = ev;
	rBX = (uint16_t)m.buttons;
	rCX = (uint16_t)m.x;
	rDX = (uint16_t)m.y;
	rSI = (uint16_t)m.raw_x;
	rDI = (uint16_t)m.raw_y;
	sCS = m.handler_seg;
	cpu.ip = m.handler_off;
	return 1;
}

void mouse_event_return(void)
{
	/* restore what the handler may have changed, then return like an IRET */
	uint16_t sp = rSP;
	Cpu c = m.saved;
	c.icount = cpu.icount;
	c.deadline = cpu.deadline;
	c.bail = cpu.bail;
	cpu = c;
	rSP = sp;
	cpu.ip = pop16();
	sCS = pop16();
	flags_set(pop16());
	m.cb_active = 0;
}

/* ---- INT 33h ------------------------------------------------------------------------- */

/*
 * Scorch waits for input by polling the mouse (and the keyboard) in a tight
 * loop, as DOS games did; a handheld should not burn its battery on that.
 * A poll is "idle" when nothing is drawn and little code ran since the last
 * one and no input is waiting: then sleep until the next timer interrupt,
 * like a HLT. Time-based behaviour is unchanged (the game's clocks advance
 * through the interrupts); only the number of empty loop rounds drops.
 *
 * But only once the game has been idle for a few of the kernel's sleep steps:
 * a short timed wait (the aiming loop polls through a few ms per step) must
 * not stretch to a whole step, about 9 ms on the Mini (0.3.2: held aiming
 * ran at a fifth of its speed there and Shift made no difference).
 */
#define IDLE_INSNS 20000
#define IDLE_MAX_US 2000
#define IDLE_STREAK_MIN 2000u
#define IDLE_STREAK_MAX 50000u

extern uint64_t vga_write_count;
int kbd_pending(void);
uint32_t kbd_events(void);
int irq_waiting(void);

static void poll_throttle(void)
{
	static uint64_t last_icount, last_vga, idle_since;
	static uint32_t last_activity, last_keys;
	static int off = -1, stats = -1;
	uint64_t now = time_us();

	if (off < 0) {
		off = getenv("SCORCH_NOTHROTTLE") != NULL;
		stats = getenv("SCORCH_POLLSTATS") != NULL;
	}
	if (stats)
		LOG("poll di=%llu dvga=%llu t=%llu", (unsigned long long)(cpu.icount - last_icount),
		    (unsigned long long)(vga_write_count - last_vga), (unsigned long long)now);
	/* idle: no new input, nothing drawn, little code since the last poll */
	if (!off && m.activity == last_activity && kbd_events() == last_keys && !kbd_pending() &&
	    vga_write_count == last_vga && cpu.icount - last_icount < IDLE_INSNS) {
		uint64_t streak = sleep_step_us * 4;
		if (streak < IDLE_STREAK_MIN)
			streak = IDLE_STREAK_MIN;
		if (streak > IDLE_STREAK_MAX)
			streak = IDLE_STREAK_MAX;
		if (!idle_since) {
			idle_since = now + 1;
		} else if (now + 1 - idle_since >= streak) {
			/* bring the timer up to date first: an overdue tick is delivered
			 * now, otherwise sleep until the next one */
			pit_update(now);
			if (!irq_waiting()) {
				uint64_t t = pit_next_irq_us();
				if (t > now + IDLE_MAX_US)
					t = now + IDLE_MAX_US;
				time_sleep_until_us(t);
			}
		}
	} else {
		idle_since = 0;
	}
	last_icount = cpu.icount;
	last_vga = vga_write_count;
	last_activity = m.activity;
	last_keys = kbd_events();
}

void mouse_int33(void)
{
	int b;
	static int logged;

	if (mcfg.log_io && logged < 200) {
		logged++;
		LOG("int33 ax=%04x bx=%04x cx=%04x dx=%04x es=%04x", rAX, rBX, rCX, rDX, sES);
	}

	switch (rAX) {
	case 0x00: case 0x21:
		reset();
		rAX = 0xFFFF;
		rBX = 2;
		break;
	case 0x01:
		if (m.show < 0)
			m.show++;
		break;
	case 0x02:
		m.show--;
		break;
	case 0x03:
		poll_throttle();
		rBX = (uint16_t)m.buttons;
		rCX = (uint16_t)m.x;
		rDX = (uint16_t)m.y;
		break;
	case 0x04:
		m.x = (int16_t)rCX;
		m.y = (int16_t)rDX;
		clamp();
		break;
	case 0x05: case 0x06: {
		int fn = rAX;
		b = rBX < 3 ? rBX : 0;
		rAX = (uint16_t)m.buttons;
		if (fn == 0x05) {
			rBX = (uint16_t)m.press_count[b];
			rCX = (uint16_t)m.press_x[b];
			rDX = (uint16_t)m.press_y[b];
			m.press_count[b] = 0;
		} else {
			rBX = (uint16_t)m.release_count[b];
			rCX = (uint16_t)m.release_x[b];
			rDX = (uint16_t)m.release_y[b];
			m.release_count[b] = 0;
		}
		break;
	}
	case 0x07:
		m.minx = (int16_t)rCX;
		m.maxx = (int16_t)rDX;
		if (m.minx > m.maxx) { int t = m.minx; m.minx = m.maxx; m.maxx = t; }
		clamp();
		break;
	case 0x08:
		m.miny = (int16_t)rCX;
		m.maxy = (int16_t)rDX;
		if (m.miny > m.maxy) { int t = m.miny; m.miny = m.maxy; m.maxy = t; }
		clamp();
		break;
	case 0x09: case 0x0A:
		break;
	case 0x0B:
		poll_throttle();
		rCX = (uint16_t)m.mick_x;
		rDX = (uint16_t)m.mick_y;
		m.mick_x = m.mick_y = 0;
		break;
	case 0x0C:
		m.handler_mask = rCX;
		m.handler_seg = sES;
		m.handler_off = rDX;
		break;
	case 0x0F:
		m.ratio_x = rCX ? rCX : 8;
		m.ratio_y = rDX ? rDX : 16;
		break;
	case 0x10: case 0x13: case 0x1C: case 0x1D:
		if (rAX == 0x1D)
			m.page = rBX;
		break;
	case 0x14: {
		uint16_t mask = m.handler_mask, seg = m.handler_seg, off = m.handler_off;
		m.handler_mask = rCX;
		m.handler_seg = sES;
		m.handler_off = rDX;
		rCX = mask;
		sES = seg;
		rDX = off;
		break;
	}
	case 0x15:
		rBX = 64;
		break;
	case 0x16: case 0x17:
		break;
	case 0x1A:
		break;
	case 0x1B:
		rBX = 50;
		rCX = 50;
		rDX = 50;
		break;
	case 0x1E:
		rBX = (uint16_t)m.page;
		break;
	case 0x24:
		/* a serial mouse on COM1 (IRQ 4): Scorch turns its "Bios Keyboard" option on
		 * for a PS/2 mouse (type 4), and only its own keyboard handler gives Shift
		 * (slow) and Alt (one step) aiming and smooth held keys */
		rBX = 0x0626;
		rCX = 0x0204;
		break;
	default:
		break;
	}
}
