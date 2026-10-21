/*
 * Timed input for tests, on the emulated clock (milliseconds):
 *
 *   # comment
 *   500 key enter            tap a key (down, up 80 ms later)
 *   500 key lshift down      hold / release
 *   900 type Player One      type text (shift added as needed)
 *   1200 mouse 320 240       pointer to game pixel (x, y)
 *   1250 mickeys 50 -20      relative mouse motion
 *   1300 click left          or right
 *   1500 pad A UP            Miyoo buttons held from now on ("pad" alone: none)
 *   2000 shot out.ppm        screenshot
 *   9000 quit
 */
#include "machine.h"
#include "platform.h"
#include "ui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void write_ppm(const char *path);

typedef struct {
	uint64_t ms;
	char cmd[12];
	char arg[160];
} Ev;

static Ev *evs;
static int nev, cur;
static uint32_t script_pad;

typedef struct {
	uint64_t ms;
	int xt;
	int down;
	int buttons; /* -1: key event */
} Pending;
static Pending pend[512];
static int npend;

static const struct {
	const char *name;
	int xt;
} keys[] = {
	{"esc", 0x01}, {"bksp", 0x0E}, {"backspace", 0x0E}, {"tab", 0x0F}, {"enter", 0x1C}, {"ctrl", 0x1D},
	{"lshift", 0x2A}, {"shift", 0x2A}, {"rshift", 0x36}, {"alt", 0x38}, {"space", 0x39}, {"caps", 0x3A},
	{"f1", 0x3B}, {"f2", 0x3C}, {"f3", 0x3D}, {"f4", 0x3E}, {"f5", 0x3F}, {"f6", 0x40}, {"f7", 0x41},
	{"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44}, {"f11", 0x57}, {"f12", 0x58},
	{"home", 0x47}, {"up", 0x48}, {"pgup", 0x49}, {"left", 0x4B}, {"right", 0x4D}, {"end", 0x4F},
	{"down", 0x50}, {"pgdn", 0x51}, {"ins", 0x52}, {"del", 0x53},
	{"minus", 0x0C}, {"equals", 0x0D}, {"comma", 0x33}, {"period", 0x34}, {"slash", 0x35},
};

/* character -> scan code, with 0x100 = needs shift */
static int char_code(char c)
{
	static const char row1[] = "1234567890-=", row1s[] = "!@#$%^&*()_+";
	static const char qrow[] = "qwertyuiop[]", arow[] = "asdfghjkl;'`", zrow[] = "zxcvbnm,./";
	static const char qrows[] = "QWERTYUIOP{}", arows[] = "ASDFGHJKL:\"~", zrows[] = "ZXCVBNM<>?";
	const char *p;
	if (c == ' ')
		return 0x39;
	if ((p = strchr(row1, c)) && c)
		return 0x02 + (int)(p - row1);
	if ((p = strchr(row1s, c)) && c)
		return 0x102 + (int)(p - row1s);
	if ((p = strchr(qrow, c)) && c)
		return 0x10 + (int)(p - qrow);
	if ((p = strchr(qrows, c)) && c)
		return 0x110 + (int)(p - qrows);
	if ((p = strchr(arow, c)) && c)
		return 0x1E + (int)(p - arow);
	if ((p = strchr(arows, c)) && c)
		return 0x11E + (int)(p - arows);
	if ((p = strchr(zrow, c)) && c)
		return 0x2C + (int)(p - zrow);
	if ((p = strchr(zrows, c)) && c)
		return 0x12C + (int)(p - zrows);
	if (c == '\\')
		return 0x2B;
	if (c == '|')
		return 0x12B;
	return -1;
}

static int key_code(const char *name)
{
	unsigned i;
	if (strlen(name) == 1) {
		int c = char_code((char)tolower((unsigned char)name[0]));
		return c >= 0 ? c & 0xFF : -1;
	}
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (!strcasecmp(keys[i].name, name))
			return keys[i].xt;
	return -1;
}

static void later(uint64_t ms, int xt, int down, int buttons)
{
	if (npend < (int)(sizeof(pend) / sizeof(pend[0]))) {
		pend[npend].ms = ms;
		pend[npend].xt = xt;
		pend[npend].down = down;
		pend[npend].buttons = buttons;
		npend++;
	}
}

static int cmp_ev(const void *a, const void *b)
{
	const Ev *x = a, *y = b;
	return x->ms < y->ms ? -1 : x->ms > y->ms;
}

static void add(uint64_t ms, const char *cmd, const char *arg)
{
	evs = realloc(evs, sizeof(Ev) * (size_t)(nev + 1));
	evs[nev].ms = ms;
	snprintf(evs[nev].cmd, sizeof(evs[nev].cmd), "%s", cmd);
	snprintf(evs[nev].arg, sizeof(evs[nev].arg), "%s", arg);
	nev++;
	qsort(evs, (size_t)nev, sizeof(Ev), cmp_ev);
}

void script_shot_at(uint64_t ms, const char *file)
{
	add(ms, "shot", file);
}

void script_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	if (!f) {
		LOG("script %s: cannot open", path);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char cmd[32] = "", *arg;
		unsigned long long ms;
		int n;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0])
			continue;
		if (sscanf(line, "%llu %31s %n", &ms, cmd, &n) < 2)
			continue;
		arg = line + n;
		add(ms, cmd, arg);
	}
	fclose(f);
}

static uint32_t pad_bits(const char *arg)
{
	static const char *const names[PAD_COUNT] = {
		"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SELECT", "START", "MENU",
	};
	char buf[160], *tok, *save = NULL;
	uint32_t bits = 0;
	snprintf(buf, sizeof(buf), "%s", arg);
	for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		int i;
		for (i = 0; i < PAD_COUNT; i++)
			if (!strcasecmp(tok, names[i]))
				bits |= PAD_BIT(i);
	}
	return bits;
}

uint32_t script_pad_state(void)
{
	return script_pad;
}

static void run(const Ev *e, uint64_t now)
{
	if (!strcmp(e->cmd, "key")) {
		char name[32] = "", how[16] = "";
		int xt;
		sscanf(e->arg, "%31s %15s", name, how);
		xt = key_code(name);
		if (xt < 0) {
			LOG("script: unknown key %s", name);
			return;
		}
		if (!strcmp(how, "down")) {
			kbd_key(xt, 1);
		} else if (!strcmp(how, "up")) {
			kbd_key(xt, 0);
		} else {
			kbd_key(xt, 1);
			later(now + 80, xt, 0, -1);
		}
	} else if (!strcmp(e->cmd, "type")) {
		const char *s = e->arg;
		uint64_t t = now;
		for (; *s; s++) {
			int c = char_code(*s);
			if (c < 0)
				continue;
			if (c & 0x100)
				later(t, 0x2A, 1, -1);
			later(t, c & 0xFF, 1, -1);
			later(t + 60, c & 0xFF, 0, -1);
			if (c & 0x100)
				later(t + 60, 0x2A, 0, -1);
			t += 120;
		}
	} else if (!strcmp(e->cmd, "mouse")) {
		int x = 0, y = 0;
		sscanf(e->arg, "%d %d", &x, &y);
		mouse_set_pos(x, y);
	} else if (!strcmp(e->cmd, "mickeys")) {
		int x = 0, y = 0;
		sscanf(e->arg, "%d %d", &x, &y);
		mouse_move(x, y);
	} else if (!strcmp(e->cmd, "click")) {
		int b = strstr(e->arg, "right") ? 2 : 1;
		mouse_buttons(b);
		later(now + 100, 0, 0, 0);
	} else if (!strcmp(e->cmd, "pad")) {
		script_pad = pad_bits(e->arg);
	} else if (!strcmp(e->cmd, "shot")) {
		/* render now so the shot shows the current state */
		Framebuffer *fb = plat_framebuffer();
		vga_update_picture();
		vga_render(fb->pixels, fb->w, fb->h, fb->pitch);
		ui_draw(fb);
		write_ppm(e->arg);
	} else if (!strcmp(e->cmd, "quit")) {
		machine_exit(0);
	} else {
		LOG("script: unknown command %s", e->cmd);
	}
}

void script_poll(uint64_t now)
{
	for (;;) {
		/* the earliest due event first; same-time events in the order queued */
		int i, best = -1;
		for (i = 0; i < npend; i++)
			if (pend[i].ms <= now && (best < 0 || pend[i].ms < pend[best].ms))
				best = i;
		if (best < 0)
			break;
		if (pend[best].buttons >= 0)
			mouse_buttons(pend[best].buttons);
		else
			kbd_key(pend[best].xt, pend[best].down);
		memmove(&pend[best], &pend[best + 1], sizeof(pend[0]) * (size_t)(npend - best - 1));
		npend--;
	}
	while (cur < nev && evs[cur].ms <= now)
		run(&evs[cur++], now);
}
