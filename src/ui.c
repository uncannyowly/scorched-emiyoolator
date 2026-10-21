/*
 * Handheld controls.
 *
 *   D-pad        arrow keys (aim: left/right = angle, up/down = power;
 *                menus; the shop's marker)
 *   A            Space (fire, buy)            B   Esc (back, cancel, done)
 *   X            Tab (next weapon)            Y   Enter (OK, Done)
 *   L1 (hold)    Shift: slower aiming; with X the previous weapon
 *   R1 (hold)    Alt: aim one step per press
 *   L2 / R2      Page Down / Page Up (fast power, shop pages)
 *   START        F1 (the game's system menu)
 *   SELECT       tap: on-screen keyboard (D-pad picks, A types, X backspace,
 *                Y shift, START Enter, B or SELECT closes)
 *                hold: mouse pointer (D-pad moves it; A / B click left / right)
 *   MENU         tap: mouse click (left button) where the pointer is
 *                hold two seconds: quit at once (main.c)
 *
 * Keys auto-repeat like a PC keyboard (500 ms, then about 11 per second).
 */
#include "ui.h"
#include "machine.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ui_update runs on the emulation thread, ui_draw may run on the render thread */
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

extern const uint8_t vga_font_8x16[4096];
void vga_layout(int w, int h, int *ox, int *oy, int *dw, int *dh);

#define K_LSHIFT 0x2A
#define K_ALT 0x38
#define K_CTRL 0x1D
/* the grey cursor keys (E0 prefix): the keypad ones turn into digits while Shift is
 * held, and Shift + arrow is the game's slow aim */
#define GREY 0xE000
#define REPEAT_DELAY_US 500000
#define REPEAT_US 92000

static int handheld;
static uint32_t prev_pad;
static int dirty;
static uint64_t last_us;

/* PC keys currently held down because of the pad (by make code, without GREY) */
static uint8_t held[128];
static uint64_t repeat_at[128];

/* ---- toast messages ------------------------------------------------------------------ */

static char toast[96];
static uint64_t toast_until;

static void show_toast(const char *s, uint64_t now)
{
	snprintf(toast, sizeof(toast), "%s", s);
	toast_until = now + 3000000;
	dirty = 1;
}

/* ---- pointer --------------------------------------------------------------------------- */

static int ptr_buttons;
static uint64_t ptr_hold_since;
static double ptr_acc_x, ptr_acc_y;

void ui_pointer_from_screen(int x, int y)
{
	Framebuffer *fb = plat_framebuffer();
	int ox, oy, dw, dh, gw = vga_mode_width(), gh = vga_mode_height();
	vga_layout(fb->w, fb->h, &ox, &oy, &dw, &dh);
	if (dw <= 0 || dh <= 0)
		return;
	x = (x - ox) * gw / dw;
	y = (y - oy) * gh / dh;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= gw) x = gw - 1;
	if (y >= gh) y = gh - 1;
	mouse_set_pos(x, y);
}

/*
 * The D-pad steers the game's own pointer, which moves by the mouse's
 * motion counters (mickeys) times its Mouse Rate (0.5 by default, so two
 * mickeys per pixel). A tap moves one pixel; holding glides slowly after
 * 150 ms and speeds up from 0.4 s to 240 pixels a second (scaled to the
 * mode's width). Fractions are kept, so slow movement stays smooth.
 */
#define MICKEYS_PER_PIXEL 2.0

static void pointer_move(uint32_t pad, uint32_t pressed, double dt, uint64_t now)
{
	const uint32_t dirs = PAD_BIT(PAD_UP) | PAD_BIT(PAD_DOWN) | PAD_BIT(PAD_LEFT) | PAD_BIT(PAD_RIGHT);
	int dx = !!(pad & PAD_BIT(PAD_RIGHT)) - !!(pad & PAD_BIT(PAD_LEFT));
	int dy = !!(pad & PAD_BIT(PAD_DOWN)) - !!(pad & PAD_BIT(PAD_UP));
	double scale = vga_mode_width() > 0 ? vga_mode_width() / 320.0 : 1.0, held, speed;
	int mx, my;

	if (!dx && !dy) {
		ptr_hold_since = 0;
		ptr_acc_x = ptr_acc_y = 0;
		return;
	}
	if (pressed & dirs) {
		/* a tap: exactly one pixel */
		mouse_move((int)(dx * MICKEYS_PER_PIXEL), (int)(dy * MICKEYS_PER_PIXEL));
		ptr_hold_since = now;
		ptr_acc_x = ptr_acc_y = 0;
		dirty = 1;
		return;
	}
	held = (now - ptr_hold_since) / 1e6;
	if (held < 0.15)
		return;
	speed = held < 0.4 ? 40.0 : 40.0 + (held - 0.4) * 200.0;
	if (speed > 240.0)
		speed = 240.0;
	speed *= scale * MICKEYS_PER_PIXEL * dt;
	ptr_acc_x += dx * speed;
	ptr_acc_y += dy * speed;
	mx = (int)ptr_acc_x;
	my = (int)ptr_acc_y;
	ptr_acc_x -= mx;
	ptr_acc_y -= my;
	if (mx || my) {
		mouse_move(mx, my);
		dirty = 1;
	}
}

/* ---- on-screen keyboard ------------------------------------------------------------- */

typedef struct {
	const char *label;
	int xt;  /* 0 = sticky modifier given by `mod` */
	int mod; /* 1 shift, 2 alt, 3 ctrl */
} OskKey;

#define OSK_ROWS 4
#define OSK_COLS 15
static const OskKey osk[OSK_ROWS][OSK_COLS] = {
	{ {"Esc", 0x01, 0}, {"F1", 0x3B, 0}, {"1", 0x02, 0}, {"2", 0x03, 0}, {"3", 0x04, 0}, {"4", 0x05, 0},
	  {"5", 0x06, 0}, {"6", 0x07, 0}, {"7", 0x08, 0}, {"8", 0x09, 0}, {"9", 0x0A, 0}, {"0", 0x0B, 0},
	  {"-", 0x0C, 0}, {"=", 0x0D, 0}, {"Bks", 0x0E, 0} },
	{ {"Tab", 0x0F, 0}, {"Q", 0x10, 0}, {"W", 0x11, 0}, {"E", 0x12, 0}, {"R", 0x13, 0}, {"T", 0x14, 0},
	  {"Y", 0x15, 0}, {"U", 0x16, 0}, {"I", 0x17, 0}, {"O", 0x18, 0}, {"P", 0x19, 0}, {"[", 0x1A, 0},
	  {"]", 0x1B, 0}, {"Ent", 0x1C, 0}, {NULL, 0, 0} },
	{ {"Shf", 0, 1}, {"A", 0x1E, 0}, {"S", 0x1F, 0}, {"D", 0x20, 0}, {"F", 0x21, 0}, {"G", 0x22, 0},
	  {"H", 0x23, 0}, {"J", 0x24, 0}, {"K", 0x25, 0}, {"L", 0x26, 0}, {";", 0x27, 0}, {"'", 0x28, 0},
	  {"Alt", 0, 2}, {"Ctl", 0, 3}, {NULL, 0, 0} },
	{ {"PgU", GREY | 0x49, 0}, {"Z", 0x2C, 0}, {"X", 0x2D, 0}, {"C", 0x2E, 0}, {"V", 0x2F, 0}, {"B", 0x30, 0},
	  {"N", 0x31, 0}, {"M", 0x32, 0}, {",", 0x33, 0}, {".", 0x34, 0}, {"/", 0x35, 0}, {"Spc", 0x39, 0},
	  {"PgD", GREY | 0x51, 0}, {"Del", GREY | 0x53, 0}, {NULL, 0, 0} },
};

static int osk_open, osk_row, osk_col, osk_top;
static int select_used; /* SELECT did something else: no keyboard on its release */
static uint32_t swallowed; /* used by the keyboard or the mouse: not keys until released */
static int osk_mods; /* bit 0 shift, 1 alt, 2 ctrl (sticky, for the next key) */
static uint64_t osk_repeat_at;

static int row_len(int r)
{
	int n = 0;
	while (n < OSK_COLS && osk[r][n].label)
		n++;
	return n;
}

/* a typed key: pressed now, released by tap_release() a little later */
static int tap_xt = -1, tap_mods;
static uint64_t tap_release_at;

static void tap_release(void)
{
	if (tap_xt < 0)
		return;
	kbd_key(tap_xt, 0);
	if (tap_mods & 4)
		kbd_key(K_CTRL, 0);
	if (tap_mods & 2)
		kbd_key(K_ALT, 0);
	if (tap_mods & 1)
		kbd_key(K_LSHIFT, 0);
	tap_xt = -1;
}

static void tap(int xt, int mods, uint64_t now)
{
	tap_release();
	if (mods & 1)
		kbd_key(K_LSHIFT, 1);
	if (mods & 2)
		kbd_key(K_ALT, 1);
	if (mods & 4)
		kbd_key(K_CTRL, 1);
	kbd_key(xt, 1);
	tap_xt = xt;
	tap_mods = mods;
	tap_release_at = now + 80000;
}

static void osk_press(uint64_t now)
{
	const OskKey *k = &osk[osk_row][osk_col];
	if (!k->xt) {
		osk_mods ^= 1 << (k->mod - 1);
	} else {
		tap(k->xt, osk_mods, now);
		osk_mods = 0;
	}
	dirty = 1;
}

static void osk_update(uint32_t pad, uint32_t pressed, uint64_t now)
{
	int moved = 0;
	uint32_t dirs = pad & (PAD_BIT(PAD_UP) | PAD_BIT(PAD_DOWN) | PAD_BIT(PAD_LEFT) | PAD_BIT(PAD_RIGHT));

	if (pressed & dirs) {
		moved = 1;
		osk_repeat_at = now + 350000;
	} else if (dirs && now >= osk_repeat_at) {
		moved = 1;
		osk_repeat_at = now + 90000;
	}
	if (moved) {
		if (pad & PAD_BIT(PAD_LEFT))
			osk_col--;
		if (pad & PAD_BIT(PAD_RIGHT))
			osk_col++;
		if (pad & PAD_BIT(PAD_UP))
			osk_row = (osk_row + OSK_ROWS - 1) % OSK_ROWS;
		if (pad & PAD_BIT(PAD_DOWN))
			osk_row = (osk_row + 1) % OSK_ROWS;
		if (osk_col < 0)
			osk_col = row_len(osk_row) - 1;
		if (osk_col >= row_len(osk_row))
			osk_col = (pad & PAD_BIT(PAD_RIGHT)) ? 0 : row_len(osk_row) - 1;
		dirty = 1;
	}
	if (pressed & PAD_BIT(PAD_A))
		osk_press(now);
	if (pressed & PAD_BIT(PAD_X))
		tap(0x0E, 0, now);
	if (pressed & PAD_BIT(PAD_Y)) {
		osk_mods ^= 1;
		dirty = 1;
	}
	if (pressed & PAD_BIT(PAD_START))
		tap(0x1C, 0, now);
	if (pressed & (PAD_BIT(PAD_L1) | PAD_BIT(PAD_R1))) {
		osk_top ^= 1;
		dirty = 1;
	}
	if (pressed & (PAD_BIT(PAD_B) | PAD_BIT(PAD_SELECT))) {
		if (pressed & PAD_BIT(PAD_SELECT))
			select_used = 1;
		osk_open = 0;
		osk_mods = 0;
		dirty = 1;
	}
}

/* ---- key mapping --------------------------------------------------------------------- */

static void set_key(int xt, int down, int repeat, uint64_t now)
{
	int k = xt & 0x7F;
	if (down && !held[k]) {
		kbd_key(xt, 1);
		held[k] = 1;
		repeat_at[k] = repeat ? now + REPEAT_DELAY_US : 0;
	} else if (down && repeat && repeat_at[k] && now >= repeat_at[k]) {
		kbd_key(xt, 1); /* typematic repeat: another make code */
		repeat_at[k] += REPEAT_US;
		if (repeat_at[k] < now)
			repeat_at[k] = now + REPEAT_US;
	} else if (!down && held[k]) {
		kbd_key(xt, 0);
		held[k] = 0;
	}
}

static void ui_update_locked(uint32_t pad, uint64_t now);

void ui_update(uint32_t pad, uint64_t now)
{
	pthread_mutex_lock(&ui_mutex);
	ui_update_locked(pad, now);
	pthread_mutex_unlock(&ui_mutex);
}

static void ui_update_locked(uint32_t pad, uint64_t now)
{
	uint32_t pressed = pad & ~prev_pad, released = prev_pad & ~pad, real = pad;
	double dt = last_us ? (now - last_us) / 1e6 : 0;
	int pointer_mode, keys;

	last_us = now;
	if (tap_xt >= 0 && now >= tap_release_at)
		tap_release();
	if (!handheld) {
		prev_pad = pad;
		return;
	}
	swallowed &= pad;
	if (osk_open) {
		osk_update(pad, pressed, now);
		swallowed |= pad;
	} else if ((released & PAD_BIT(PAD_SELECT)) && !select_used) {
		osk_open = 1;
		show_toast("A type  X backspace  Y shift  START enter  B close", now);
		dirty = 1;
	} else if (pressed & PAD_BIT(PAD_SELECT)) {
		select_used = 0;
	}
	pointer_mode = !osk_open && (real & PAD_BIT(PAD_SELECT));
	if (pointer_mode) {
		/* SELECT steered the pointer: its release must not open the keyboard */
		if (real & (PAD_BIT(PAD_UP) | PAD_BIT(PAD_DOWN) | PAD_BIT(PAD_LEFT) | PAD_BIT(PAD_RIGHT) |
		            PAD_BIT(PAD_A) | PAD_BIT(PAD_B) | PAD_BIT(PAD_MENU)))
			select_used = 1;
		swallowed |= real;
	}
	keys = !osk_open && !pointer_mode;
	pad &= ~swallowed | PAD_BIT(PAD_L1) | PAD_BIT(PAD_R1) | PAD_BIT(PAD_SELECT);

	set_key(GREY | 0x48, keys && (pad & PAD_BIT(PAD_UP)), 1, now);
	set_key(GREY | 0x50, keys && (pad & PAD_BIT(PAD_DOWN)), 1, now);
	set_key(GREY | 0x4B, keys && (pad & PAD_BIT(PAD_LEFT)), 1, now);
	set_key(GREY | 0x4D, keys && (pad & PAD_BIT(PAD_RIGHT)), 1, now);
	set_key(0x39, keys && (pad & PAD_BIT(PAD_A)), 1, now);  /* Space */
	set_key(0x01, keys && (pad & PAD_BIT(PAD_B)), 0, now);  /* Esc */
	set_key(0x0F, keys && (pad & PAD_BIT(PAD_X)), 1, now);  /* Tab */
	set_key(0x1C, keys && (pad & PAD_BIT(PAD_Y)), 0, now);  /* Enter */
	set_key(GREY | 0x51, keys && (pad & PAD_BIT(PAD_L2)), 1, now); /* Page Down */
	set_key(GREY | 0x49, keys && (pad & PAD_BIT(PAD_R2)), 1, now); /* Page Up */
	set_key(0x3B, keys && (pad & PAD_BIT(PAD_START)), 0, now); /* F1 */
	/* modifiers work with the pointer too */
	set_key(K_LSHIFT, !osk_open && (pad & PAD_BIT(PAD_L1)), 0, now);
	set_key(K_ALT, !osk_open && (pad & PAD_BIT(PAD_R1)), 0, now);

	/* mouse buttons: MENU is the left button anywhere (steer the pointer with
	 * SELECT + D-pad first); with SELECT held, A and B click left and right.
	 * Holding MENU for two seconds still quits (main.c). */
	{
		int b = (real & PAD_BIT(PAD_MENU)) ? 1 : 0;
		if (pointer_mode) {
			if (real & PAD_BIT(PAD_A))
				b |= 1;
			if (real & PAD_BIT(PAD_B))
				b |= 2;
			pointer_move(real, pressed, dt, now);
		} else {
			pointer_move(0, 0, dt, now);
		}
		if (b != ptr_buttons) {
			ptr_buttons = b;
			mouse_buttons(b);
		}
	}
	if (toast[0] && now >= toast_until) {
		toast[0] = 0;
		dirty = 1;
	}
	prev_pad = real;
}

/* ---- drawing ---------------------------------------------------------------------------- */

static void blend_rect(Framebuffer *fb, int x0, int y0, int w, int h, uint32_t col, int alpha)
{
	int x, y;
	for (y = y0; y < y0 + h; y++) {
		uint32_t *p;
		if (y < 0 || y >= fb->h)
			continue;
		p = fb->pixels + (size_t)y * fb->pitch;
		for (x = x0; x < x0 + w; x++) {
			uint32_t d, r, g, b;
			if (x < 0 || x >= fb->w)
				continue;
			d = p[x];
			r = (((d >> 16) & 255) * (256 - alpha) + ((col >> 16) & 255) * alpha) >> 8;
			g = (((d >> 8) & 255) * (256 - alpha) + ((col >> 8) & 255) * alpha) >> 8;
			b = ((d & 255) * (256 - alpha) + (col & 255) * alpha) >> 8;
			p[x] = (r << 16) | (g << 8) | b;
		}
	}
}

static void fill_rect(Framebuffer *fb, int x0, int y0, int w, int h, uint32_t col)
{
	int x, y;
	if (x0 < 0) {
		w += x0;
		x0 = 0;
	}
	if (x0 + w > fb->w)
		w = fb->w - x0;
	for (y = y0; y < y0 + h; y++) {
		uint32_t *p;
		if (y < 0 || y >= fb->h || w <= 0)
			continue;
		p = fb->pixels + (size_t)y * fb->pitch + x0;
		for (x = 0; x < w; x++)
			p[x] = col;
	}
}

static void draw_text(Framebuffer *fb, int x, int y, const char *s, uint32_t col, int scale)
{
	for (; *s; s++, x += 8 * scale) {
		int cy, cx;
		for (cy = 0; cy < 16 * scale; cy++) {
			uint8_t bits = vga_font_8x16[(uint8_t)*s * 16 + cy / scale];
			int py = y + cy;
			if (py < 0 || py >= fb->h)
				continue;
			for (cx = 0; cx < 8 * scale; cx++) {
				int px = x + cx;
				if (px >= 0 && px < fb->w && (bits & (0x80 >> (cx / scale))))
					fb->pixels[(size_t)py * fb->pitch + px] = col;
			}
		}
	}
}

static void draw_pointer(Framebuffer *fb)
{
	static const char *const arrow[] = {
		"X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
		"X........X", "X.....XXXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
	};
	int px, py, vis, ox, oy, dw, dh, gw = vga_mode_width(), gh = vga_mode_height();
	int x0, y0, r;

	/* only the driver's own cursor: Scorch draws its pointer itself from the
	 * mouse's motion counters */
	mouse_get_screen_pos(&px, &py, &vis);
	if (!vis)
		return;
	if (gw <= 0 || gh <= 0)
		return;
	vga_layout(fb->w, fb->h, &ox, &oy, &dw, &dh);
	x0 = ox + px * dw / gw;
	y0 = oy + py * dh / gh;
	for (r = 0; r < (int)(sizeof(arrow) / sizeof(arrow[0])); r++) {
		int c;
		for (c = 0; arrow[r][c]; c++) {
			int x = x0 + c, y = y0 + r;
			if (x < 0 || y < 0 || x >= fb->w || y >= fb->h || arrow[r][c] == ' ')
				continue;
			fb->pixels[(size_t)y * fb->pitch + x] = arrow[r][c] == 'X' ? 0x000000 : 0xFFFFFF;
		}
	}
}

static void draw_osk(Framebuffer *fb)
{
	int cw = fb->w / OSK_COLS, ch = 34, r, c;
	int top = osk_top ? 4 : fb->h - OSK_ROWS * ch - 4;

	fill_rect(fb, 0, top - 4, fb->w, OSK_ROWS * ch + 8, 0x101820);
	for (r = 0; r < OSK_ROWS; r++) {
		int n = row_len(r), x0 = (fb->w - n * cw) / 2;
		for (c = 0; c < n; c++) {
			const OskKey *k = &osk[r][c];
			int x = x0 + c * cw, y = top + r * ch;
			int sel = r == osk_row && c == osk_col;
			int on = !k->xt && (osk_mods & (1 << (k->mod - 1)));
			const char *label = k->label;
			char lower[2];
			int len, scale;
			fill_rect(fb, x + 2, y + 2, cw - 4, ch - 4, sel ? 0xE0A020 : on ? 0x3070C0 : 0x404850);
			if (strlen(label) == 1 && label[0] >= 'A' && label[0] <= 'Z' && !(osk_mods & 1)) {
				lower[0] = (char)(label[0] + 32);
				lower[1] = 0;
				label = lower;
			}
			len = (int)strlen(label);
			scale = len == 1 ? 2 : 1;
			draw_text(fb, x + (cw - len * 8 * scale) / 2, y + (ch - 16 * scale) / 2, label,
			          sel ? 0x000000 : 0xF0F0F0, scale);
		}
	}
}

void ui_draw(Framebuffer *fb)
{
	pthread_mutex_lock(&ui_mutex);
	if (osk_open)
		draw_osk(fb);
	draw_pointer(fb);
	if (toast[0]) {
		int w = (int)strlen(toast) * 8 + 16;
		int x = (fb->w - w) / 2, y = osk_open && osk_top ? fb->h - 28 : 6;
		blend_rect(fb, x, y, w, 22, 0x000000, 190);
		draw_text(fb, x + 8, y + 3, toast, 0xFFFFA0, 1);
	}
	dirty = 0;
	pthread_mutex_unlock(&ui_mutex);
}

int ui_needs_redraw(void)
{
	return dirty;
}

void ui_init(int hh)
{
	handheld = hh;
	if (handheld)
		show_toast("A fire  Y OK  B back  X weapon  SELECT keys  MENU click", 0);
}
