/*
 * 8253/8254 timer (ports 40h-43h), port 61h and the PC speaker.
 *
 * Channel 0 drives IRQ 0 at its programmed rate. Ticks that pass while an
 * earlier one is still pending are owed and delivered back to back, so the
 * game's tick counters keep pace with real time.
 *
 * The speaker is channel 2's square wave gated by port 61h bits 0-1. Every
 * change is time-stamped and queued; the audio thread replays the queue a
 * fixed latency behind the emulation, so even short clicks keep their length.
 */
#include "machine.h"

#include <math.h>

#define PIT_HZ 1193182.0

typedef struct {
	uint16_t reload;  /* 0 means 65536 */
	uint8_t mode, rw, bcd;
	uint8_t write_hi; /* next write is the high byte */
	uint8_t read_hi;
	uint8_t latched;
	uint16_t latch;
	uint64_t start_ns; /* when the count was loaded */
} Channel;

static Channel ch[3];
static uint8_t port61;
static uint64_t next_irq_ns;
static int owed;
uint64_t pit_dropped; /* real time: ticks given up (statistics) */

int pic_requested(int irq);

static double period_ns(int c)
{
	uint32_t r = ch[c].reload ? ch[c].reload : 65536u;
	return r * 1e9 / PIT_HZ;
}

static uint16_t current_count(int c)
{
	uint32_t r = ch[c].reload ? ch[c].reload : 65536u;
	uint64_t elapsed = (uint64_t)((double)(time_ns() - ch[c].start_ns) * PIT_HZ / 1e9);
	uint32_t ph = (uint32_t)(elapsed % r);
	if (ch[c].mode == 3) {
		/* square wave: counts down by two, twice per period */
		uint32_t half = r / 2 ? r / 2 : 1;
		return (uint16_t)((r - 2 * (ph % half)) & ~1u);
	}
	return (uint16_t)(r - ph);
}

/* ---- speaker events --------------------------------------------------------------- */

typedef struct {
	uint64_t t;
	uint16_t reload;
	uint8_t on;
} SpkEvent;

#define SPK_Q 4096
static SpkEvent spk_q[SPK_Q];
static volatile unsigned spk_head, spk_tail; /* written by emulation / audio thread */
static uint16_t spk_reload = 0;
static uint8_t spk_on;

static void speaker_changed(void)
{
	uint8_t on = (port61 & 3) == 3 && ch[2].mode == 3;
	uint16_t r = ch[2].reload;
	unsigned next;

	if (on == spk_on && (!on || r == spk_reload))
		return;
	spk_on = on;
	spk_reload = r;
	next = (spk_head + 1) % SPK_Q;
	if (next == spk_tail)
		return; /* queue full: drop */
	spk_q[spk_head].t = time_ns();
	spk_q[spk_head].reload = r;
	spk_q[spk_head].on = on;
	__atomic_store_n(&spk_head, next, __ATOMIC_RELEASE);
}

/* audio thread state */
static struct {
	uint64_t t;      /* emulated ns at the next sample */
	uint16_t reload;
	uint8_t on;
	double phase;
	double lp;       /* low-pass state */
} aud;

#define LATENCY_NS 50000000ull

void speaker_render(int16_t *out, int frames, int rate)
{
	double dt = 1e9 / rate;
	uint64_t now = time_ns();
	uint64_t target = now > LATENCY_NS ? now - LATENCY_NS : 0;
	uint64_t span = (uint64_t)(dt * frames);
	int i;

	/* (re)synchronise if the audio clock drifted away from the emulation */
	if (aud.t + span + 200000000ull < target || aud.t > target + 200000000ull)
		aud.t = target > span ? target - span : 0;
	for (i = 0; i < frames; i++) {
		double s = 0, f;
		unsigned tail = spk_tail;
		while (tail != __atomic_load_n(&spk_head, __ATOMIC_ACQUIRE) && spk_q[tail].t <= aud.t) {
			aud.reload = spk_q[tail].reload;
			aud.on = spk_q[tail].on;
			tail = (tail + 1) % SPK_Q;
		}
		spk_tail = tail;
		if (aud.on) {
			f = PIT_HZ / (aud.reload ? aud.reload : 65536);
			if (f < 18000) {
				aud.phase += f / rate;
				aud.phase -= floor(aud.phase);
				s = aud.phase < 0.5 ? 1.0 : -1.0;
			}
		}
		/* the speaker cone is no brick wall: soften the edges a little */
		aud.lp += (s - aud.lp) * 0.45;
		{
			int v = (int)(aud.lp * 7000);
			out[i * 2] = (int16_t)v;
			out[i * 2 + 1] = (int16_t)v;
		}
		aud.t += (uint64_t)dt;
	}
}

/* ---- timer --------------------------------------------------------------------------- */

static uint8_t pit_in(uint16_t port)
{
	int c = port & 3;
	uint16_t v;

	if (port == 0x61) {
		/* bit 4 toggles with memory refresh (15 us), bit 5 is channel 2's output */
		uint8_t r = port61 & 0x0F;
		uint64_t t = time_ns();
		if ((t / 15085) & 1)
			r |= 0x10;
		if (ch[2].mode == 3) {
			double p = period_ns(2);
			if (fmod((double)(t - ch[2].start_ns), p) < p / 2)
				r |= 0x20;
		}
		return r;
	}
	if (c == 3)
		return 0xFF;
	if (ch[c].latched) {
		v = ch[c].latch;
	} else {
		v = current_count(c);
	}
	if (ch[c].rw == 1) {
		ch[c].latched = 0;
		return (uint8_t)v;
	}
	if (ch[c].rw == 2) {
		ch[c].latched = 0;
		return (uint8_t)(v >> 8);
	}
	if (!ch[c].read_hi) {
		ch[c].read_hi = 1;
		if (ch[c].latched)
			return (uint8_t)v;
		ch[c].latch = v;
		ch[c].latched = 1;
		return (uint8_t)v;
	}
	ch[c].read_hi = 0;
	ch[c].latched = 0;
	return (uint8_t)(v >> 8);
}

static void reload_done(int c)
{
	ch[c].start_ns = time_ns();
	if (c == 0) {
		next_irq_ns = ch[0].start_ns + (uint64_t)period_ns(0);
		if (ch[0].reload != 0)
			LOG("pit: channel 0 at %.2f Hz", PIT_HZ / ch[0].reload);
	}
	if (c == 2)
		speaker_changed();
}

static void pit_out(uint16_t port, uint8_t v)
{
	int c = port & 3;

	if (port == 0x61) {
		port61 = v;
		speaker_changed();
		return;
	}
	if (c == 3) {
		int sc = v >> 6, rw = (v >> 4) & 3;
		if (sc == 3)
			return; /* read-back */
		if (rw == 0) {
			ch[sc].latch = current_count(sc);
			ch[sc].latched = 1;
			ch[sc].read_hi = 0;
			return;
		}
		ch[sc].rw = (uint8_t)rw;
		ch[sc].mode = (v >> 1) & 7;
		if (ch[sc].mode > 5)
			ch[sc].mode -= 4;
		ch[sc].bcd = v & 1;
		ch[sc].write_hi = 0;
		ch[sc].read_hi = 0;
		ch[sc].latched = 0;
		if (sc == 2)
			speaker_changed();
		return;
	}
	switch (ch[c].rw) {
	case 1:
		ch[c].reload = v;
		reload_done(c);
		break;
	case 2:
		ch[c].reload = (uint16_t)(v << 8);
		reload_done(c);
		break;
	default:
		if (!ch[c].write_hi) {
			ch[c].reload = (uint16_t)((ch[c].reload & 0xFF00) | v);
			ch[c].write_hi = 1;
		} else {
			ch[c].reload = (uint16_t)((ch[c].reload & 0x00FF) | (v << 8));
			ch[c].write_hi = 0;
			reload_done(c);
		}
		break;
	}
}

static void advance(uint64_t now_us, int limit)
{
	uint64_t now = now_us * 1000;
	uint64_t p = (uint64_t)period_ns(0);
	int add = 0;

	if (mcfg.realtime && now > next_irq_ns + 1000000000ull) {
		pit_dropped += (now - next_irq_ns) / p;
		next_irq_ns = now; /* far behind (the device slept): skip ahead */
	}
	while (now >= next_irq_ns) {
		add++;
		next_irq_ns += p;
	}
	/* in real time, new ticks beyond `limit` owed are dropped; virtual time
	 * delivers every one (runs stay reproducible) */
	if (mcfg.realtime && owed + add > limit) {
		int keep = owed < limit ? limit - owed : 0;
		pit_dropped += (uint64_t)(add - keep);
		add = keep;
	}
	owed += add;
	if (owed > 0 && !pic_requested(0)) {
		pic_raise(0);
		owed--;
	}
}

/* a machine that cannot keep up drops ticks (beyond 8 owed) instead of storming */
void pit_update(uint64_t now_us)
{
	advance(now_us, 8);
}

/* after a sleep in real time: every tick that fell due meanwhile is owed, up to
 * 100 ms of them. A kernel may round a 0.2 ms sleep up to its 10 ms tick; with
 * the cap of 8 that lost most ticks (0.3.1 on the Mini: timer at 36%). */
void pit_update_slept(uint64_t now_us)
{
	int limit = (int)(100000000.0 / period_ns(0));
	advance(now_us, limit > 8 ? limit : 8);
}

uint64_t pit_next_irq_us(void)
{
	if (owed > 0)
		return time_us();
	return next_irq_ns / 1000;
}

void pit_init(void)
{
	int c;
	for (c = 0; c < 3; c++) {
		ch[c].reload = 0;
		ch[c].mode = 3;
		ch[c].rw = 3;
	}
	ch[1].reload = 18;
	ch[2].reload = 1331;
	ch[0].start_ns = 0;
	next_irq_ns = (uint64_t)period_ns(0);
	port_register(0x40, 0x43, pit_in, pit_out);
	port_register(0x61, 0x61, pit_in, pit_out);
}

