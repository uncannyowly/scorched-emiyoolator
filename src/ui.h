/*
 * Handheld controls: maps the Miyoo's buttons onto the PC keyboard and
 * mouse, and draws the on-screen keyboard and pointer.
 */
#ifndef UI_H
#define UI_H

#include "platform.h"

#ifdef PLATFORM_MIYOO
#define PLATFORM_IS_HANDHELD 1
#else
#define PLATFORM_IS_HANDHELD 0
#endif

void ui_init(int handheld);
void ui_update(uint32_t pad, uint64_t now_us);
void ui_draw(Framebuffer *fb);
int ui_needs_redraw(void);
/* desktop mouse at framebuffer pixel (x, y) */
void ui_pointer_from_screen(int x, int y);

#endif
