/*
 * Desktop backend (SDL2), for development. With `headless` nothing is
 * opened: frames stay in memory (for screenshots) and there is no input.
 *
 * Keyboard and mouse are passed to the emulated PC. With `pad_mode` the
 * keyboard pretends to be the Miyoo's buttons instead, to try the handheld
 * controls: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L1/R1,
 * 1/2 = L2/R2, Right Shift = SELECT, Enter = START, Esc = MENU.
 */
#include "platform.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480

static Framebuffer fb;
static int headless, pad_mode;
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int quit;
static uint32_t pad;
static int mouse_mask;
static SDL_AudioDeviceID audio_dev;
static AudioCallback audio_cb;
static void *audio_user;

int plat_init(int hl, int pm)
{
	headless = hl;
	pad_mode = pm;
	fb.w = W;
	fb.h = H;
	fb.pitch = W;
	fb.pixels = calloc((size_t)W * H, 4);
	if (!fb.pixels)
		return -1;
	if (headless)
		return 0;
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return -1;
	}
	win = SDL_CreateWindow("Scorched Earth (eMiYoolator)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                       W * 2, H * 2, SDL_WINDOW_RESIZABLE);
	if (!win)
		return -1;
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!ren)
		ren = SDL_CreateRenderer(win, -1, 0);
	if (!ren)
		return -1;
	SDL_RenderSetLogicalSize(ren, W, H);
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
	SDL_ShowCursor(SDL_DISABLE);
	return tex ? 0 : -1;
}

void plat_shutdown(void)
{
	plat_audio_close();
	if (!headless)
		SDL_Quit();
	free(fb.pixels);
	fb.pixels = NULL;
}

Framebuffer *plat_framebuffer(void)
{
	return &fb;
}

void plat_present(void)
{
	if (headless)
		return;
	SDL_UpdateTexture(tex, NULL, fb.pixels, fb.pitch * 4);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
}

static int xt_code(SDL_Scancode s)
{
	static const struct {
		SDL_Scancode s;
		int xt;
	} map[] = {
		{SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_1, 0x02}, {SDL_SCANCODE_2, 0x03}, {SDL_SCANCODE_3, 0x04},
		{SDL_SCANCODE_4, 0x05}, {SDL_SCANCODE_5, 0x06}, {SDL_SCANCODE_6, 0x07}, {SDL_SCANCODE_7, 0x08},
		{SDL_SCANCODE_8, 0x09}, {SDL_SCANCODE_9, 0x0A}, {SDL_SCANCODE_0, 0x0B}, {SDL_SCANCODE_MINUS, 0x0C},
		{SDL_SCANCODE_EQUALS, 0x0D}, {SDL_SCANCODE_BACKSPACE, 0x0E}, {SDL_SCANCODE_TAB, 0x0F},
		{SDL_SCANCODE_Q, 0x10}, {SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13},
		{SDL_SCANCODE_T, 0x14}, {SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17},
		{SDL_SCANCODE_O, 0x18}, {SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0x1A},
		{SDL_SCANCODE_RIGHTBRACKET, 0x1B}, {SDL_SCANCODE_RETURN, 0x1C}, {SDL_SCANCODE_LCTRL, 0x1D},
		{SDL_SCANCODE_A, 0x1E}, {SDL_SCANCODE_S, 0x1F}, {SDL_SCANCODE_D, 0x20}, {SDL_SCANCODE_F, 0x21},
		{SDL_SCANCODE_G, 0x22}, {SDL_SCANCODE_H, 0x23}, {SDL_SCANCODE_J, 0x24}, {SDL_SCANCODE_K, 0x25},
		{SDL_SCANCODE_L, 0x26}, {SDL_SCANCODE_SEMICOLON, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0x28},
		{SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2A}, {SDL_SCANCODE_BACKSLASH, 0x2B},
		{SDL_SCANCODE_Z, 0x2C}, {SDL_SCANCODE_X, 0x2D}, {SDL_SCANCODE_C, 0x2E}, {SDL_SCANCODE_V, 0x2F},
		{SDL_SCANCODE_B, 0x30}, {SDL_SCANCODE_N, 0x31}, {SDL_SCANCODE_M, 0x32}, {SDL_SCANCODE_COMMA, 0x33},
		{SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35}, {SDL_SCANCODE_RSHIFT, 0x36},
		{SDL_SCANCODE_KP_MULTIPLY, 0x37}, {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39},
		{SDL_SCANCODE_CAPSLOCK, 0x3A}, {SDL_SCANCODE_F1, 0x3B}, {SDL_SCANCODE_F2, 0x3C},
		{SDL_SCANCODE_F3, 0x3D}, {SDL_SCANCODE_F4, 0x3E}, {SDL_SCANCODE_F5, 0x3F}, {SDL_SCANCODE_F6, 0x40},
		{SDL_SCANCODE_F7, 0x41}, {SDL_SCANCODE_F8, 0x42}, {SDL_SCANCODE_F9, 0x43}, {SDL_SCANCODE_F10, 0x44},
		{SDL_SCANCODE_NUMLOCKCLEAR, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x46}, {SDL_SCANCODE_KP_7, 0x47},
		{SDL_SCANCODE_KP_8, 0x48}, {SDL_SCANCODE_KP_9, 0x49}, {SDL_SCANCODE_KP_MINUS, 0x4A},
		{SDL_SCANCODE_KP_4, 0x4B}, {SDL_SCANCODE_KP_5, 0x4C}, {SDL_SCANCODE_KP_6, 0x4D},
		{SDL_SCANCODE_KP_PLUS, 0x4E}, {SDL_SCANCODE_KP_1, 0x4F}, {SDL_SCANCODE_KP_2, 0x50},
		{SDL_SCANCODE_KP_3, 0x51}, {SDL_SCANCODE_KP_0, 0x52}, {SDL_SCANCODE_KP_PERIOD, 0x53},
		{SDL_SCANCODE_F11, 0x57}, {SDL_SCANCODE_F12, 0x58},
		/* the grey keys send the keypad codes without NumLock (no E0 prefix):
		 * the most compatible form for DOS games */
		{SDL_SCANCODE_HOME, 0x47}, {SDL_SCANCODE_UP, 0x48}, {SDL_SCANCODE_PAGEUP, 0x49},
		{SDL_SCANCODE_LEFT, 0x4B}, {SDL_SCANCODE_RIGHT, 0x4D}, {SDL_SCANCODE_END, 0x4F},
		{SDL_SCANCODE_DOWN, 0x50}, {SDL_SCANCODE_PAGEDOWN, 0x51}, {SDL_SCANCODE_INSERT, 0x52},
		{SDL_SCANCODE_DELETE, 0x53}, {SDL_SCANCODE_KP_ENTER, 0x1C}, {SDL_SCANCODE_RCTRL, 0x1D},
		{SDL_SCANCODE_RALT, 0x38}, {SDL_SCANCODE_KP_DIVIDE, 0x35},
	};
	unsigned i;
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].s == s)
			return map[i].xt;
	return -1;
}

static int pad_button(SDL_Scancode s)
{
	switch (s) {
	case SDL_SCANCODE_UP: return PAD_UP;
	case SDL_SCANCODE_DOWN: return PAD_DOWN;
	case SDL_SCANCODE_LEFT: return PAD_LEFT;
	case SDL_SCANCODE_RIGHT: return PAD_RIGHT;
	case SDL_SCANCODE_X: return PAD_A;
	case SDL_SCANCODE_Z: return PAD_B;
	case SDL_SCANCODE_S: return PAD_X;
	case SDL_SCANCODE_A: return PAD_Y;
	case SDL_SCANCODE_Q: return PAD_L1;
	case SDL_SCANCODE_W: return PAD_R1;
	case SDL_SCANCODE_1: return PAD_L2;
	case SDL_SCANCODE_2: return PAD_R2;
	case SDL_SCANCODE_RSHIFT: return PAD_SELECT;
	case SDL_SCANCODE_RETURN: return PAD_START;
	case SDL_SCANCODE_ESCAPE: return PAD_MENU;
	default: return -1;
	}
}

uint32_t plat_poll(const PlatInput *in)
{
	SDL_Event e;

	if (headless)
		return pad;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			quit = 1;
			break;
		case SDL_KEYDOWN: case SDL_KEYUP: {
			int down = e.type == SDL_KEYDOWN;
			if (e.key.repeat)
				break;
			if (pad_mode) {
				int b = pad_button(e.key.keysym.scancode);
				if (b >= 0) {
					if (down)
						pad |= PAD_BIT(b);
					else
						pad &= ~PAD_BIT(b);
				}
			} else if (in && in->key) {
				int xt = xt_code(e.key.keysym.scancode);
				if (xt >= 0)
					in->key(xt, down);
			}
			break;
		}
		case SDL_MOUSEMOTION:
			if (in && in->mouse_abs)
				in->mouse_abs(e.motion.x, e.motion.y);
			break;
		case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
			int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 :
			          e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
			if (e.type == SDL_MOUSEBUTTONDOWN)
				mouse_mask |= bit;
			else
				mouse_mask &= ~bit;
			if (in && in->mouse_buttons)
				in->mouse_buttons(mouse_mask);
			break;
		}
		default:
			break;
		}
	}
	return pad;
}

int plat_quit_requested(void)
{
	return quit;
}

static void sdl_audio(void *user, Uint8 *stream, int len)
{
	(void)user;
	audio_cb((int16_t *)stream, len / 4, audio_user);
}

int plat_audio_open(int rate, AudioCallback cb, void *user)
{
	SDL_AudioSpec want, have;

	if (headless)
		return -1;
	audio_cb = cb;
	audio_user = user;
	memset(&want, 0, sizeof(want));
	want.freq = rate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	want.callback = sdl_audio;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!audio_dev)
		return -1;
	SDL_PauseAudioDevice(audio_dev, 0);
	return 0;
}

void plat_audio_close(void)
{
	if (audio_dev) {
		SDL_CloseAudioDevice(audio_dev);
		audio_dev = 0;
	}
}
