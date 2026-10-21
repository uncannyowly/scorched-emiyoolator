/*
 * Platform layer: one 640x480 screen, a button pad (Miyoo) or keyboard and
 * mouse (desktop), and one audio stream.
 *
 * Backends: platform_sdl.c (desktop, SDL2, or headless for tests) and
 * platform_miyoo.c (/dev/fb0, evdev buttons, MI_AO audio).
 * Pixels are XRGB8888.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Linux input.h defines BTN_* macros: our button bits are PAD_* */
enum {
	PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
	PAD_A, PAD_B, PAD_X, PAD_Y,
	PAD_L1, PAD_R1, PAD_L2, PAD_R2,
	PAD_SELECT, PAD_START, PAD_MENU,
	PAD_COUNT
};

#define PAD_BIT(b) (1u << (b))

typedef struct {
	uint32_t *pixels;
	int w, h;
	int pitch; /* in pixels */
} Framebuffer;

typedef void (*AudioCallback)(int16_t *out, int frames, void *user);

/* desktop input goes straight to the emulated PC */
typedef struct {
	void (*key)(int xt_scancode, int down);  /* 0xE0xx for extended keys */
	void (*mouse_abs)(int x, int y);         /* framebuffer pixels */
	void (*mouse_buttons)(int mask);
} PlatInput;

int plat_init(int headless, int pad_mode);
void plat_shutdown(void);
Framebuffer *plat_framebuffer(void);
void plat_present(void);
/* returns the held pad buttons (Miyoo, or the desktop's pad emulation) */
uint32_t plat_poll(const PlatInput *in);
int plat_quit_requested(void);
int plat_audio_open(int rate, AudioCallback cb, void *user);
void plat_audio_close(void);

#endif
