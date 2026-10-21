/*
 * VGA and VESA: memory, registers, video BIOS (INT 10h) and rendering.
 *
 * Standard VGA memory is four 64 KB planes stored interleaved
 * (vram[offset * 4 + plane]); the write/read pipeline (map mask, set/reset,
 * rotate, logical op, bit mask, latches, read modes) is implemented so the
 * unchained "Mode X" tricks Fastgraph uses for 320x240, 360x480 etc. work.
 * The displayed geometry comes from the CRTC registers.
 *
 * VESA modes (VBE 1.2: 640x400, 640x480, 800x600, 1024x768 at 8 bpp) use
 * the same buffer linearly through a 64 KB window at A000 with 64 KB
 * granularity; the window function is the HLE entry F000:0180.
 */
#include "machine.h"
#include "ops.h"

#include <stdlib.h>

#define VRAM_SIZE (1u << 20)

extern const uint8_t vga_font_8x16[4096];
uint16_t bios_font_offset(int which);

enum { KIND_TEXT, KIND_PLANAR, KIND_VESA };

static struct {
	uint8_t vram[VRAM_SIZE + 4];
	uint8_t kind;
	uint8_t mode;           /* BIOS mode number */
	uint16_t vesa_mode;     /* 0 or 0x100.. */
	int vesa_w, vesa_h, vesa_pitch;
	uint32_t vesa_start;    /* display start (bytes) */
	uint32_t bank;          /* window A position in bytes */

	uint8_t seq_index, seq[8];
	uint8_t gc_index, gc[16];
	uint8_t crtc_index, crtc[32];
	uint8_t attr_index, attr[32], attr_flip;
	uint8_t misc;
	uint8_t latch[4];

	uint8_t dac[256][3];
	uint8_t dac_mask;
	uint8_t dac_write, dac_read, dac_comp, dac_state;
	uint32_t pal[256];
	int pal_dirty;
	int dirty;
	uint64_t frames;
} vga;

/* ---- palette ---------------------------------------------------------------------- */

static void pal_update(void)
{
	int i;
	for (i = 0; i < 256; i++) {
		uint8_t r = vga.dac[i][0] & 63, g = vga.dac[i][1] & 63, b = vga.dac[i][2] & 63;
		vga.pal[i] = 0xFF000000u | ((uint32_t)((r << 2) | (r >> 4)) << 16) |
		             ((uint32_t)((g << 2) | (g >> 4)) << 8) | (uint32_t)((b << 2) | (b >> 4));
	}
	vga.pal_dirty = 0;
}

/* the standard VGA palette after a mode set: EGA colours, grey ramp, colour cube */
static void pal_default(void)
{
	static const uint8_t ega[16][3] = {
		{0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42}, {42, 0, 0}, {42, 0, 42}, {42, 21, 0}, {42, 42, 42},
		{21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
	};
	static const uint8_t grey[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
	/* (low, high) component levels for 3 intensities x 3 saturations */
	static const uint8_t lohi[3][3][2] = {
		{ {0, 63}, {31, 63}, {45, 63} }, { {0, 28}, {14, 28}, {20, 28} }, { {0, 16}, {8, 16}, {11, 16} },
	};
	int i;

	memset(vga.dac, 0, sizeof(vga.dac));
	for (i = 0; i < 16; i++)
		memcpy(vga.dac[i], ega[i], 3);
	for (i = 0; i < 16; i++)
		vga.dac[16 + i][0] = vga.dac[16 + i][1] = vga.dac[16 + i][2] = grey[i];
	/* 24 hues around the colour wheel for each intensity and saturation */
	for (i = 32; i < 248; i++) {
		int k = i - 32, block = k / 72, sat = (k % 72) / 24, hue = k % 24;
		int L = lohi[block][sat][0], H = lohi[block][sat][1];
		int pos = hue % 4, up = L + (H - L) * pos / 4, down = H - (H - L) * pos / 4;
		int rgb[3];
		switch (hue / 4) {
		case 0: rgb[0] = up; rgb[1] = L; rgb[2] = H; break;
		case 1: rgb[0] = H; rgb[1] = L; rgb[2] = down; break;
		case 2: rgb[0] = H; rgb[1] = up; rgb[2] = L; break;
		case 3: rgb[0] = down; rgb[1] = H; rgb[2] = L; break;
		case 4: rgb[0] = L; rgb[1] = H; rgb[2] = up; break;
		default: rgb[0] = L; rgb[1] = down; rgb[2] = H; break;
		}
		vga.dac[i][0] = (uint8_t)rgb[0];
		vga.dac[i][1] = (uint8_t)rgb[1];
		vga.dac[i][2] = (uint8_t)rgb[2];
	}
	vga.pal_dirty = 1;
}

/* ---- memory --------------------------------------------------------------------------- */

static inline int chain4(void)
{
	return (vga.seq[4] & 0x08) != 0;
}

/* CPU address -> offset in the window, or -1 if the address is not mapped */
static inline int32_t window_offset(uint32_t addr)
{
	switch ((vga.gc[6] >> 2) & 3) {
	case 0: return (int32_t)(addr - 0xA0000);
	case 1: return addr < 0xB0000 ? (int32_t)(addr - 0xA0000) : -1;
	case 2: return addr >= 0xB0000 && addr < 0xB8000 ? (int32_t)(addr - 0xB0000) : -1;
	default: return addr >= 0xB8000 ? (int32_t)(addr - 0xB8000) : -1;
	}
}

uint64_t vga_write_count;

uint8_t vga_mem_read8(uint32_t addr)
{
	int32_t off;

	if (vga.kind == KIND_VESA) {
		if (addr >= 0xB0000)
			return mem[addr];
		return vga.vram[(vga.bank + (addr - 0xA0000)) & (VRAM_SIZE - 1)];
	}
	if (vga.kind == KIND_TEXT) {
		if (addr >= 0xB8000)
			return vga.vram[addr - 0xB8000];
		return mem[addr];
	}
	off = window_offset(addr);
	if (off < 0)
		return mem[addr];
	if (chain4()) {
		uint32_t i = (((uint32_t)off & ~3u) << 2) | ((uint32_t)off & 3);
		return vga.vram[i & (VRAM_SIZE - 1)];
	}
	off &= 0xFFFF;
	memcpy(vga.latch, &vga.vram[(uint32_t)off * 4], 4);
	if (vga.gc[5] & 0x08) {
		/* read mode 1: colour compare */
		uint8_t res = 0xFF;
		int p;
		for (p = 0; p < 4; p++) {
			if (vga.gc[7] & (1 << p)) {
				uint8_t want = (vga.gc[2] & (1 << p)) ? 0xFF : 0x00;
				res &= (uint8_t)~(vga.latch[p] ^ want);
			}
		}
		return res;
	}
	return vga.latch[vga.gc[4] & 3];
}

void vga_mem_write8(uint32_t addr, uint8_t v)
{
	int32_t off;
	uint8_t mask, rot, data[4];
	int p, mode;

	vga_write_count++;
	if (vga.kind == KIND_VESA) {
		if (addr >= 0xB0000) {
			mem[addr] = v;
			return;
		}
		vga.vram[(vga.bank + (addr - 0xA0000)) & (VRAM_SIZE - 1)] = v;
		vga.dirty = 1;
		return;
	}
	if (vga.kind == KIND_TEXT) {
		if (addr >= 0xB8000) {
			vga.vram[addr - 0xB8000] = v;
			vga.dirty = 1;
		} else {
			mem[addr] = v;
		}
		return;
	}
	off = window_offset(addr);
	if (off < 0) {
		mem[addr] = v;
		return;
	}
	vga.dirty = 1;
	mode = vga.gc[5] & 3;
	if (mode == 0 && (vga.gc[3] & 0x1F) == 0 && vga.gc[1] == 0 && vga.gc[8] == 0xFF) {
		/* plain writes: mode 13h, and Mode X drawing (the planes in the map mask) */
		if (chain4()) {
			uint32_t i = (((uint32_t)off & ~3u) << 2) | ((uint32_t)off & 3);
			if (vga.seq[2] & (1 << (off & 3)))
				vga.vram[i & (VRAM_SIZE - 1)] = v;
		} else {
			uint8_t *d = &vga.vram[((uint32_t)off & 0xFFFF) * 4];
			uint8_t mm = vga.seq[2];
			if (mm == 0x0F) {
				d[0] = d[1] = d[2] = d[3] = v;
			} else {
				if (mm & 1) d[0] = v;
				if (mm & 2) d[1] = v;
				if (mm & 4) d[2] = v;
				if (mm & 8) d[3] = v;
			}
		}
		return;
	}
	rot = (uint8_t)(vga.gc[3] & 7);
	if (rot)
		v = (uint8_t)((v >> rot) | (v << (8 - rot)));
	mask = vga.gc[8];
	for (p = 0; p < 4; p++) {
		uint8_t d;
		switch (mode) {
		case 0:
			d = (vga.gc[1] & (1 << p)) ? ((vga.gc[0] & (1 << p)) ? 0xFF : 0x00) : v;
			break;
		case 1:
			data[p] = vga.latch[p];
			continue;
		case 2:
			d = (v & (1 << p)) ? 0xFF : 0x00;
			break;
		default:
			d = (vga.gc[0] & (1 << p)) ? 0xFF : 0x00;
			break;
		}
		if (mode != 3) {
			switch ((vga.gc[3] >> 3) & 3) {
			case 1: d &= vga.latch[p]; break;
			case 2: d |= vga.latch[p]; break;
			case 3: d ^= vga.latch[p]; break;
			default: break;
			}
			data[p] = (uint8_t)((d & mask) | (vga.latch[p] & ~mask));
		} else {
			uint8_t m = (uint8_t)(v & mask);
			data[p] = (uint8_t)((d & m) | (vga.latch[p] & ~m));
		}
	}
	if (chain4()) {
		uint32_t i = (((uint32_t)off & ~3u) << 2) | ((uint32_t)off & 3);
		if (vga.seq[2] & (1 << (off & 3)))
			vga.vram[i & (VRAM_SIZE - 1)] = data[off & 3];
		return;
	}
	off &= 0xFFFF;
	for (p = 0; p < 4; p++)
		if (vga.seq[2] & (1 << p))
			vga.vram[(uint32_t)off * 4 + p] = data[p];
}

/* ---- registers --------------------------------------------------------------------------- */

static uint32_t frame_period_ns(void)
{
	/* 480-line and VESA modes refresh at 60 Hz, the 400-line ones at 70 Hz */
	uint32_t vde = vga.crtc[0x12] | ((vga.crtc[7] & 2) << 7) | ((vga.crtc[7] & 0x40) << 3);
	if (vga.kind == KIND_VESA || vde >= 450)
		return 16683333u;
	return 14268607u;
}

/*
 * In play the game spends about 90% of every frame in a tight loop on 3DAh,
 * waiting for the vertical retrace. Sleep through most of that wait, never so
 * long that the kernel's slow wake-up (about 9 ms on the Mini) could miss the
 * retrace; and if a wake-up is late anyway, show the retrace it slept through
 * once, so the game does not lose a frame.
 */
static uint64_t spin_icount, spin_vga, retrace_owed_ns;
static int spin;

static uint8_t status1(void)
{
	uint64_t t = time_ns();
	uint32_t period = frame_period_ns();
	uint32_t phase = (uint32_t)(t % period);
	uint32_t vr_start = period - period / 12, vr_len = period / 40;
	uint8_t s = 0;

	vga.attr_flip = 0;
	/* reading this port again and again, drawing nothing: a retrace wait */
	if (cpu.icount - spin_icount < 64 && vga_write_count == spin_vga)
		spin++;
	else
		spin = 0;
	spin_icount = cpu.icount;
	spin_vga = vga_write_count;
	if (spin >= 32 && phase < vr_start) {
		/* waiting for this frame's retrace (in the blanking after one, a loop
		 * may wait for something else: no sleeping there); wake up early
		 * enough for a slow wake-up */
		uint64_t next = t - phase + vr_start;
		uint64_t margin_us = sleep_step_us + sleep_step_us / 2;
		if ((next - t) / 1000 > margin_us + 100) {
			time_sleep_until_us(next / 1000 - margin_us);
			retrace_owed_ns = next;
			t = time_ns();
			phase = (uint32_t)(t % period);
		}
	}
	/* vertical blank at the end of the frame, retrace in its first part */
	if (phase >= vr_start) {
		s |= 0x01;
		if (phase < vr_start + vr_len)
			s |= 0x08;
	} else if ((t / 1000) % 32 >= 26) {
		s |= 0x01; /* horizontal blanking, roughly */
	}
	if (retrace_owed_ns && t >= retrace_owed_ns) {
		s |= 0x09; /* slept into or past it: shown once, maybe late */
		retrace_owed_ns = 0;
	}
	return s;
}

static uint8_t vga_in(uint16_t port)
{
	switch (port) {
	case 0x3C0: return vga.attr_index;
	case 0x3C1: return vga.attr[vga.attr_index & 0x1F];
	case 0x3C2: return 0x10; /* input status 0: switch sense */
	case 0x3C3: return 1;
	case 0x3C4: return vga.seq_index;
	case 0x3C5: return vga.seq_index < 5 ? vga.seq[vga.seq_index] : 0xFF;
	case 0x3C6: return vga.dac_mask;
	case 0x3C7: return vga.dac_state;
	case 0x3C8: return vga.dac_write;
	case 0x3C9: {
		uint8_t v = vga.dac[vga.dac_read][vga.dac_comp];
		if (++vga.dac_comp == 3) {
			vga.dac_comp = 0;
			vga.dac_read++;
		}
		return v;
	}
	case 0x3CA: return 0;
	case 0x3CC: return vga.misc;
	case 0x3CE: return vga.gc_index;
	case 0x3CF: return vga.gc_index < 9 ? vga.gc[vga.gc_index] : 0xFF;
	case 0x3D4: case 0x3B4: return vga.crtc_index;
	case 0x3D5: case 0x3B5: return vga.crtc_index < 0x19 ? vga.crtc[vga.crtc_index] : 0xFF;
	case 0x3DA: case 0x3BA: return status1();
	default: return 0xFF;
	}
}

static void vga_out(uint16_t port, uint8_t v)
{
	switch (port) {
	case 0x3C0:
		if (!vga.attr_flip) {
			vga.attr_index = v & 0x3F;
		} else {
			if ((vga.attr_index & 0x1F) < 0x15)
				vga.attr[vga.attr_index & 0x1F] = v;
			vga.dirty = 1;
		}
		vga.attr_flip ^= 1;
		break;
	case 0x3C2: vga.misc = v; break;
	case 0x3C4: vga.seq_index = v & 7; break;
	case 0x3C5:
		if (vga.seq_index < 5)
			vga.seq[vga.seq_index] = v;
		break;
	case 0x3C6: vga.dac_mask = v; vga.pal_dirty = 1; vga.dirty = 1; break;
	case 0x3C7: vga.dac_read = v; vga.dac_comp = 0; vga.dac_state = 0; break;
	case 0x3C8: vga.dac_write = v; vga.dac_comp = 0; vga.dac_state = 3; break;
	case 0x3C9:
		vga.dac[vga.dac_write][vga.dac_comp] = v & 63;
		if (++vga.dac_comp == 3) {
			vga.dac_comp = 0;
			vga.dac_write++;
		}
		vga.pal_dirty = 1;
		vga.dirty = 1;
		break;
	case 0x3CE: vga.gc_index = v & 15; break;
	case 0x3CF:
		if (vga.gc_index < 9)
			vga.gc[vga.gc_index] = v;
		break;
	case 0x3D4: case 0x3B4: vga.crtc_index = v & 0x3F; break;
	case 0x3D5: case 0x3B5:
		if (vga.crtc_index < 0x19) {
			/* registers 0-7 are write protected by bit 7 of register 11h */
			if (vga.crtc_index <= 7 && (vga.crtc[0x11] & 0x80)) {
				if (vga.crtc_index == 7)
					vga.crtc[7] = (uint8_t)((vga.crtc[7] & ~0x10) | (v & 0x10));
				break;
			}
			vga.crtc[vga.crtc_index] = v;
			vga.dirty = 1;
		}
		break;
	default:
		break;
	}
}

/* ---- modes ----------------------------------------------------------------------------- */

/* register sets of the standard modes (from the IBM VGA tables) */
static const uint8_t crtc_13h[25] = {
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF,
};
static const uint8_t crtc_03h[25] = {
	0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
	0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF,
};
static const uint8_t crtc_12h[25] = {
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF,
};

typedef struct {
	uint16_t mode;
	int w, h;
} VesaMode;

static const VesaMode vesa_modes[] = {
	{ 0x100, 640, 400 }, { 0x101, 640, 480 }, { 0x103, 800, 600 }, { 0x105, 1024, 768 },
};

static void set_bda_mode(uint8_t mode, int cols, int rows, int charh)
{
	wr8(0x449, mode);
	wr16(0x44A, (uint16_t)cols);
	wr8(0x484, (uint8_t)(rows - 1));
	wr16(0x485, (uint16_t)charh);
	wr16(0x44C, (uint16_t)(mode == 3 ? 4096 : 0));
	wr16(0x44E, 0);
	wr16(0x450, 0);
	wr8(0x462, 0);
}

static void set_mode(uint8_t mode, int clear)
{
	int i;

	vga.vesa_mode = 0;
	vga.vesa_start = 0;
	vga.bank = 0;
	memset(vga.seq, 0, sizeof(vga.seq));
	memset(vga.gc, 0, sizeof(vga.gc));
	memset(vga.attr, 0, sizeof(vga.attr));
	vga.seq[2] = 0x0F;
	vga.gc[7] = 0x0F;
	vga.gc[8] = 0xFF;
	vga.dac_mask = 0xFF;
	for (i = 0; i < 16; i++)
		vga.attr[i] = (uint8_t)i;
	vga.attr[0x12] = 0x0F;
	vga.mode = mode & 0x7F;
	switch (vga.mode) {
	case 0x13:
		vga.kind = KIND_PLANAR;
		vga.seq[1] = 0x01;
		vga.seq[4] = 0x0E;
		vga.gc[5] = 0x40;
		vga.gc[6] = 0x05;
		vga.attr[0x10] = 0x41;
		vga.misc = 0x63;
		memcpy(vga.crtc, crtc_13h, sizeof(crtc_13h));
		set_bda_mode(0x13, 40, 25, 8);
		break;
	case 0x12: case 0x11: case 0x10: case 0x0D: case 0x0E:
		vga.kind = KIND_PLANAR;
		vga.seq[1] = 0x01;
		vga.seq[4] = 0x06;
		vga.gc[6] = 0x05;
		vga.attr[0x10] = 0x01;
		vga.misc = 0xE3;
		memcpy(vga.crtc, crtc_12h, sizeof(crtc_12h));
		set_bda_mode(vga.mode, 80, 30, 16);
		break;
	default:
		vga.kind = KIND_TEXT;
		vga.mode = 3;
		vga.seq[1] = 0x00;
		vga.seq[4] = 0x02;
		vga.gc[5] = 0x10;
		vga.gc[6] = 0x0E;
		vga.attr[0x10] = 0x0C;
		vga.misc = 0x67;
		memcpy(vga.crtc, crtc_03h, sizeof(crtc_03h));
		set_bda_mode(3, 80, 25, 16);
		break;
	}
	if (clear) {
		if (vga.kind == KIND_TEXT) {
			for (i = 0; i < 0x8000; i += 2) {
				vga.vram[i] = ' ';
				vga.vram[i + 1] = 0x07;
			}
		} else {
			memset(vga.vram, 0, VRAM_SIZE);
		}
	}
	pal_default();
	vga.dirty = 1;
	LOG("vga: mode %02x", vga.mode);
}

static int set_vesa_mode(uint16_t m)
{
	unsigned i;
	for (i = 0; i < sizeof(vesa_modes) / sizeof(vesa_modes[0]); i++) {
		if (vesa_modes[i].mode == (m & 0x1FF)) {
			set_mode(0x13, 0);
			vga.kind = KIND_VESA;
			vga.vesa_mode = vesa_modes[i].mode;
			vga.vesa_w = vesa_modes[i].w;
			vga.vesa_h = vesa_modes[i].h;
			vga.vesa_pitch = vesa_modes[i].w;
			vga.seq[4] = 0x0E;
			vga.gc[6] = 0x05;
			if (!(m & 0x8000))
				memset(vga.vram, 0, VRAM_SIZE);
			set_bda_mode(0x13, vga.vesa_w / 8, vga.vesa_h / 16, 16);
			wr8(0x449, 0x62); /* some BIOSes store an internal number; programs ask 4F03h */
			LOG("vga: VESA mode %03x (%dx%d)", vga.vesa_mode, vga.vesa_w, vga.vesa_h);
			return 1;
		}
	}
	return 0;
}

/* ---- VESA BIOS ------------------------------------------------------------------------- */

#define VBE_INFO_OFF 0x4000
#define VBE_MODES_OFF 0x4100
#define VBE_OEM_OFF 0x4120

static void vbe_info(uint16_t seg, uint16_t off)
{
	uint32_t a = LIN(seg, off);
	unsigned i;
	for (i = 0; i < 256; i++)
		wr8(a + i, 0);
	wr8(a, 'V'); wr8(a + 1, 'E'); wr8(a + 2, 'S'); wr8(a + 3, 'A');
	wr16(a + 4, 0x0102);
	wr16(a + 6, VBE_OEM_OFF);
	wr16(a + 8, VBIOS_SEG);
	wr32(a + 10, 0);
	wr16(a + 14, VBE_MODES_OFF);
	wr16(a + 16, VBIOS_SEG);
	wr16(a + 18, 16); /* 1 MB */
}

static int vbe_mode_info(uint16_t mode, uint16_t seg, uint16_t off)
{
	uint32_t a = LIN(seg, off);
	unsigned i;
	const VesaMode *vm = NULL;

	for (i = 0; i < sizeof(vesa_modes) / sizeof(vesa_modes[0]); i++)
		if (vesa_modes[i].mode == (mode & 0x1FF))
			vm = &vesa_modes[i];
	if (!vm)
		return 0;
	for (i = 0; i < 256; i++)
		wr8(a + i, 0);
	wr16(a + 0x00, 0x001B);
	wr8(a + 0x02, 0x07);
	wr8(a + 0x03, 0x00);
	wr16(a + 0x04, 64);
	wr16(a + 0x06, 64);
	wr16(a + 0x08, 0xA000);
	wr16(a + 0x0A, 0x0000);
	wr16(a + 0x0C, 0x0180); /* window function: F000:0180 */
	wr16(a + 0x0E, BIOS_SEG);
	wr16(a + 0x10, (uint16_t)vm->w);
	wr16(a + 0x12, (uint16_t)vm->w);
	wr16(a + 0x14, (uint16_t)vm->h);
	wr8(a + 0x16, 8);
	wr8(a + 0x17, 16);
	wr8(a + 0x18, 1);
	wr8(a + 0x19, 8);
	wr8(a + 0x1A, 1);
	wr8(a + 0x1B, 4);
	wr8(a + 0x1C, 0);
	wr8(a + 0x1D, (uint8_t)(VRAM_SIZE / ((uint32_t)vm->w * vm->h) - 1));
	wr8(a + 0x1E, 1);
	return 1;
}

static void vbe_window(void)
{
	/* BH = 0 set / 1 get, BL = window, DX = position in 64 KB units */
	if (rBL != 0) {
		rAX = 0x014F;
		return;
	}
	if (rBH == 0) {
		vga.bank = ((uint32_t)rDX << 16) & (VRAM_SIZE - 1);
	} else {
		rDX = (uint16_t)(vga.bank >> 16);
	}
	rAX = 0x004F;
}

void vesa_window_call(void)
{
	vbe_window();
}

static void int10_vesa(void)
{
	switch (rAL) {
	case 0x00:
		vbe_info(sES, rDI);
		rAX = 0x004F;
		break;
	case 0x01:
		rAX = vbe_mode_info(rCX, sES, rDI) ? 0x004F : 0x014F;
		break;
	case 0x02:
		if (rBX < 0x100 || (rBX & 0x1FF) < 0x100) {
			set_mode((uint8_t)rBX, !(rBX & 0x8000));
			rAX = 0x004F;
		} else {
			rAX = set_vesa_mode(rBX) ? 0x004F : 0x014F;
		}
		break;
	case 0x03:
		rBX = vga.vesa_mode ? vga.vesa_mode : vga.mode;
		rAX = 0x004F;
		break;
	case 0x05:
		vbe_window();
		break;
	case 0x06:
		if (!vga.vesa_mode) {
			rAX = 0x014F;
			break;
		}
		if (rBL == 0 && rCX >= (uint16_t)vga.vesa_w)
			vga.vesa_pitch = rCX;
		rBX = (uint16_t)vga.vesa_pitch;
		rCX = (uint16_t)vga.vesa_pitch;
		rDX = (uint16_t)(VRAM_SIZE / (uint32_t)vga.vesa_pitch);
		rAX = 0x004F;
		break;
	case 0x07:
		if (!vga.vesa_mode) {
			rAX = 0x014F;
			break;
		}
		if (rBL == 0 || rBL == 0x80) {
			vga.vesa_start = (uint32_t)rDX * (uint32_t)vga.vesa_pitch + rCX;
			vga.dirty = 1;
		} else {
			rCX = (uint16_t)(vga.vesa_start % (uint32_t)vga.vesa_pitch);
			rDX = (uint16_t)(vga.vesa_start / (uint32_t)vga.vesa_pitch);
			rBH = 0;
		}
		rAX = 0x004F;
		break;
	case 0x08:
		rBH = 6;
		rAX = 0x004F;
		break;
	default:
		rAX = 0x014F;
		break;
	}
}

/* ---- INT 10h ------------------------------------------------------------------------------- */

static void text_putc(uint8_t ch, uint8_t attr, int use_attr)
{
	uint16_t pos = rd16(0x450);
	int x = pos & 0xFF, y = pos >> 8, cols = rd16(0x44A), rows = rd8(0x484) + 1;

	switch (ch) {
	case 7: return;
	case 8: if (x > 0) x--; break;
	case 10: y++; break;
	case 13: x = 0; break;
	default:
		if (vga.kind == KIND_TEXT) {
			vga.vram[(y * cols + x) * 2] = ch;
			if (use_attr)
				vga.vram[(y * cols + x) * 2 + 1] = attr;
			vga.dirty = 1;
		}
		x++;
		break;
	}
	if (x >= cols) {
		x = 0;
		y++;
	}
	if (y >= rows) {
		y = rows - 1;
		if (vga.kind == KIND_TEXT) {
			memmove(vga.vram, vga.vram + cols * 2, (size_t)(rows - 1) * cols * 2);
			for (x = 0; x < cols; x++) {
				vga.vram[((rows - 1) * cols + x) * 2] = ' ';
				vga.vram[((rows - 1) * cols + x) * 2 + 1] = 0x07;
			}
			x = 0;
		}
	}
	wr16(0x450, (uint16_t)((y << 8) | x));
}

/* DOS console output (INT 21h on handles 1/2, functions 2/6/9) */
void vga_tty_putc(uint8_t ch)
{
	if (ch == 9) {
		do
			text_putc(' ', 0x07, 0);
		while (rd16(0x450) & 7);
		return;
	}
	text_putc(ch, 0x07, 0);
}

void vga_int10(void)
{
	switch (rAH) {
	case 0x00:
		set_mode(rAL, !(rAL & 0x80));
		break;
	case 0x01:
		wr16(0x460, rCX);
		break;
	case 0x02:
		wr16(0x450 + (rBH & 7) * 2, rDX);
		break;
	case 0x03:
		rDX = rd16(0x450 + (rBH & 7) * 2);
		rCX = rd16(0x460);
		break;
	case 0x05:
		wr8(0x462, rAL);
		break;
	case 0x06: case 0x07:
		if (vga.kind == KIND_TEXT) {
			int top = rCH, left = rCL, bottom = rDH, right = rDL, n = rAL, x, y, cols = rd16(0x44A);
			int h = bottom - top + 1;
			if (n == 0 || n > h)
				n = h;
			for (y = 0; y < h; y++) {
				int dst = rAH == 0x06 ? top + y : bottom - y;
				int src = rAH == 0x06 ? dst + n : dst - n;
				for (x = left; x <= right && x < cols; x++) {
					uint8_t *d = &vga.vram[(dst * cols + x) * 2];
					if (y < h - n) {
						d[0] = vga.vram[(src * cols + x) * 2];
						d[1] = vga.vram[(src * cols + x) * 2 + 1];
					} else {
						d[0] = ' ';
						d[1] = rBH;
					}
				}
			}
			vga.dirty = 1;
		}
		break;
	case 0x08:
		if (vga.kind == KIND_TEXT) {
			uint16_t pos = rd16(0x450);
			int i = ((pos >> 8) * rd16(0x44A) + (pos & 0xFF)) * 2;
			rAL = vga.vram[i];
			rAH = vga.vram[i + 1];
		}
		break;
	case 0x09: case 0x0A: {
		uint16_t pos = rd16(0x450);
		int n = rCX;
		while (n-- > 0)
			text_putc(rAL, rBL, rAH == 0x09);
		wr16(0x450, pos);
		break;
	}
	case 0x0C:
		if (vga.kind != KIND_TEXT) {
			uint32_t a = 0xA0000u + (uint32_t)rDX * (vga.mode == 0x13 && !vga.vesa_mode ? 320u : 640u) + rCX;
			if (vga.kind == KIND_VESA) {
				uint32_t o = (uint32_t)rDX * (uint32_t)vga.vesa_pitch + rCX;
				vga.vram[o & (VRAM_SIZE - 1)] = rAL;
				vga.dirty = 1;
			} else {
				vga_mem_write8(a, rAL);
			}
		}
		break;
	case 0x0D:
		if (vga.kind == KIND_VESA)
			rAL = vga.vram[((uint32_t)rDX * (uint32_t)vga.vesa_pitch + rCX) & (VRAM_SIZE - 1)];
		else if (vga.kind != KIND_TEXT)
			rAL = vga_mem_read8(0xA0000u + (uint32_t)rDX * 320u + rCX);
		break;
	case 0x0E:
		text_putc(rAL, 0x07, 0);
		break;
	case 0x0F:
		rAL = vga.mode;
		rAH = (uint8_t)rd16(0x44A);
		rBH = rd8(0x462);
		break;
	case 0x10:
		switch (rAL) {
		case 0x00:
			if (rBL < 16)
				vga.attr[rBL] = rBH;
			break;
		case 0x01:
			vga.attr[0x11] = rBH;
			break;
		case 0x02: {
			int i;
			for (i = 0; i < 16; i++)
				vga.attr[i] = rd8(LIN(sES, rDX + i));
			vga.attr[0x11] = rd8(LIN(sES, rDX + 16));
			break;
		}
		case 0x03:
			break;
		case 0x07:
			rBH = vga.attr[rBL & 0x1F];
			break;
		case 0x08:
			rBH = vga.attr[0x11];
			break;
		case 0x09: {
			int i;
			for (i = 0; i < 16; i++)
				wr8(LIN(sES, rDX + i), vga.attr[i]);
			wr8(LIN(sES, rDX + 16), vga.attr[0x11]);
			break;
		}
		case 0x10:
			vga.dac[rBL][0] = rDH & 63;
			vga.dac[rBL][1] = rCH & 63;
			vga.dac[rBL][2] = rCL & 63;
			vga.pal_dirty = vga.dirty = 1;
			break;
		case 0x12: {
			int i, n = rCX, first = rBX;
			for (i = 0; i < n && first + i < 256; i++) {
				vga.dac[first + i][0] = rd8(LIN(sES, rDX + i * 3)) & 63;
				vga.dac[first + i][1] = rd8(LIN(sES, rDX + i * 3 + 1)) & 63;
				vga.dac[first + i][2] = rd8(LIN(sES, rDX + i * 3 + 2)) & 63;
			}
			vga.pal_dirty = vga.dirty = 1;
			break;
		}
		case 0x15:
			rDH = vga.dac[rBL][0];
			rCH = vga.dac[rBL][1];
			rCL = vga.dac[rBL][2];
			break;
		case 0x17: {
			int i, n = rCX, first = rBX;
			for (i = 0; i < n && first + i < 256; i++) {
				wr8(LIN(sES, rDX + i * 3), vga.dac[first + i][0]);
				wr8(LIN(sES, rDX + i * 3 + 1), vga.dac[first + i][1]);
				wr8(LIN(sES, rDX + i * 3 + 2), vga.dac[first + i][2]);
			}
			break;
		}
		case 0x18:
			vga.dac_mask = rBL;
			break;
		case 0x19:
			rBL = vga.dac_mask;
			break;
		case 0x1A:
			rBX = 0;
			break;
		default:
			break;
		}
		break;
	case 0x11:
		if (rAL == 0x30) {
			int which = rBH;
			uint16_t off;
			switch (which) {
			case 0:
				sES = rd16(0x1F * 4 + 2);
				rBP = rd16(0x1F * 4);
				break;
			case 1:
				sES = rd16(0x43 * 4 + 2);
				rBP = rd16(0x43 * 4);
				break;
			case 2: case 5:
				off = bios_font_offset(14);
				sES = VBIOS_SEG;
				rBP = off;
				break;
			case 3:
				sES = VBIOS_SEG;
				rBP = bios_font_offset(8);
				break;
			case 4:
				sES = VBIOS_SEG;
				rBP = (uint16_t)(bios_font_offset(8) + 1024);
				break;
			default:
				sES = VBIOS_SEG;
				rBP = bios_font_offset(16);
				break;
			}
			rCX = rd16(0x485);
			rDL = rd8(0x484);
		}
		break;
	case 0x12:
		if (rBL == 0x10) {
			rBH = 0;
			rBL = 3;
			rCX = 0x0009;
		} else {
			rAL = 0x12;
		}
		break;
	case 0x1A:
		if (rAL == 0) {
			rAL = 0x1A;
			rBX = 0x0008;
		}
		break;
	case 0x1B:
		rAL = 0x1B;
		break;
	case 0x4F:
		int10_vesa();
		break;
	default:
		break;
	}
}

/* ---- rendering ------------------------------------------------------------------------------- */

int vga_mode_width(void)
{
	if (vga.kind == KIND_VESA)
		return vga.vesa_w;
	if (vga.kind == KIND_TEXT)
		return 640;
	return (vga.crtc[1] + 1) * ((vga.attr[0x10] & 0x40) ? 4 : 8);
}

int vga_mode_height(void)
{
	uint32_t vde, lines, rep;
	if (vga.kind == KIND_VESA)
		return vga.vesa_h;
	if (vga.kind == KIND_TEXT)
		return 400;
	vde = vga.crtc[0x12] | ((vga.crtc[7] & 2) << 7) | ((vga.crtc[7] & 0x40) << 3);
	lines = vde + 1;
	rep = (uint32_t)(vga.crtc[9] & 0x1F) + 1;
	if (vga.crtc[9] & 0x80)
		rep *= 2;
	return (int)(lines / rep);
}

uint64_t vga_frame_counter(void)
{
	return vga.frames;
}

/* ---- the picture ------------------------------------------------------------------
 *
 * Two steps: vga_update_picture() reads the visible screen into pic[] (one
 * palette index per pixel), only when video memory, registers or palette
 * were touched, and reports whether the picture really changed (games often
 * redraw identical pixels); vga_render() scales pic[] to the screen.
 */

static uint8_t pic[PIC_MAX], pic_prev[PIC_MAX];
static uint32_t pal_prev[256];
static int pic_w, pic_h, prev_w, prev_h;

static void fetch_line(int sy, uint8_t *line, int sw)
{
	int x;

	if (vga.kind == KIND_VESA) {
		uint32_t o = vga.vesa_start + (uint32_t)sy * (uint32_t)vga.vesa_pitch;
		if (o + (uint32_t)sw <= VRAM_SIZE) {
			memcpy(line, vga.vram + o, (size_t)sw);
		} else {
			for (x = 0; x < sw; x++)
				line[x] = vga.vram[(o + (uint32_t)x) & (VRAM_SIZE - 1)];
		}
	} else if (vga.kind == KIND_TEXT) {
		int row = sy / 16, cy = sy % 16;
		for (x = 0; x < 80 && x * 8 + 7 < sw; x++) {
			uint8_t ch = vga.vram[(row * 80 + x) * 2], at = vga.vram[(row * 80 + x) * 2 + 1];
			uint8_t bits = vga_font_8x16[ch * 16 + cy];
			int b;
			for (b = 0; b < 8; b++)
				line[x * 8 + b] = (bits & (0x80 >> b)) ? (at & 15) : (at >> 4) & 7;
		}
	} else {
		uint32_t start = ((uint32_t)vga.crtc[0x0C] << 8) | vga.crtc[0x0D];
		uint32_t pitchw = (uint32_t)vga.crtc[0x13] * 2;
		int c4 = chain4(), b256 = (vga.attr[0x10] & 0x40) != 0;
		if (vga.crtc[0x14] & 0x40) {
			/* doubleword mode (mode 13h): addresses are x4 */
			uint32_t base = start * 4 + (uint32_t)sy * pitchw * 4;
			for (x = 0; x < sw; x++) {
				uint32_t a = (base + (uint32_t)x) & 0xFFFF;
				if (c4)
					line[x] = vga.vram[((a & ~3u) << 2) | (a & 3)];
				else
					line[x] = vga.vram[((a >> 2) << 2) | (a & 3)];
			}
		} else if (b256) {
			/* unchained 256 colours: four pixels per address, one per plane */
			uint32_t base = start + (uint32_t)sy * pitchw;
			if (((base + (uint32_t)(sw >> 2)) & ~0xFFFFu) == 0) {
				const uint8_t *v = vga.vram + base * 4;
				memcpy(line, v, (size_t)sw); /* plane-interleaved = pixel order */
			} else {
				for (x = 0; x < sw; x++)
					line[x] = vga.vram[(((base + (uint32_t)(x >> 2)) & 0xFFFF) << 2) | (uint32_t)(x & 3)];
			}
		} else {
			/* 16-colour planar */
			uint32_t base = start + (uint32_t)sy * pitchw;
			for (x = 0; x < sw; x++) {
				uint32_t a = ((base + (uint32_t)(x >> 3)) & 0xFFFF) << 2;
				int bit = 7 - (x & 7), c = 0, p;
				for (p = 0; p < 4; p++)
					c |= ((vga.vram[a + p] >> bit) & 1) << p;
				c &= vga.attr[0x12];
				line[x] = (uint8_t)((vga.attr[c] & 0x3F) | ((vga.attr[0x14] & 0x0C) << 4));
			}
		}
	}
}

/* returns 1 if what the screen shows changed since the last call */
int vga_update_picture(void)
{
	int sw = vga_mode_width(), sh = vga_mode_height(), y, changed;

	if (!vga.dirty && !vga.pal_dirty && sw == pic_w && sh == pic_h)
		return 0;
	vga.dirty = 0;
	if (vga.pal_dirty)
		pal_update();
	if (sw <= 0 || sh <= 0 || sw > 1024 || (long)sw * sh > PIC_MAX)
		sw = sh = 0;
	for (y = 0; y < sh; y++)
		fetch_line(y, pic + (size_t)y * sw, sw);
	pic_w = sw;
	pic_h = sh;
	changed = sw != prev_w || sh != prev_h || memcmp(pic, pic_prev, (size_t)sw * sh) != 0 ||
	          memcmp(vga.pal, pal_prev, sizeof(pal_prev)) != 0;
	if (changed) {
		memcpy(pic_prev, pic, (size_t)sw * sh);
		memcpy(pal_prev, vga.pal, sizeof(pal_prev));
		prev_w = sw;
		prev_h = sh;
	}
	return changed;
}

int vga_present_due(uint64_t now)
{
	(void)now;
	return vga_update_picture();
}

/* where a sw x sh picture goes on a w x h screen: integer scale when it
 * fits, else scaled down keeping the aspect ratio */
static void layout(int sw, int sh, int w, int h, int *ox, int *oy, int *dw, int *dh)
{
	if (sw <= 0 || sh <= 0) {
		*ox = *oy = *dw = *dh = 0;
		return;
	}
	if (sw * 2 <= w && sh * 2 <= h) {
		*dw = sw * 2;
		*dh = sh * 2;
	} else if (sw <= w && sh <= h) {
		*dw = sw;
		*dh = sh;
	} else {
		*dw = w;
		*dh = sh * w / sw;
		if (*dh > h) {
			*dh = h;
			*dw = sw * h / sh;
		}
	}
	*ox = (w - *dw) / 2;
	*oy = (h - *dh) / 2;
}

void vga_layout(int w, int h, int *ox, int *oy, int *dw, int *dh)
{
	layout(vga_mode_width(), vga_mode_height(), w, h, ox, oy, dw, dh);
}

/* a copy of the picture, for rendering on another thread */
void vga_snapshot(Picture *p)
{
	p->sw = pic_w;
	p->sh = pic_h;
	p->mask = vga.dac_mask;
	memcpy(p->pal, vga.pal, sizeof(p->pal));
	if (pic_w > 0 && pic_h > 0)
		memcpy(p->pic, pic, (size_t)pic_w * pic_h);
}

/* scales a picture onto dst (w x h); touches no emulation state */
void vga_render_picture(const Picture *p, uint32_t *dst, int w, int h, int pitch)
{
	static int xmap[2048];
	static int xmap_sw, xmap_dw;
	int sw = p->sw, sh = p->sh, ox, oy, dw, dh, x, y;
	const uint32_t *pal = p->pal;
	const uint8_t *pic_ = p->pic;
	uint8_t mask = p->mask;

	if (sw <= 0 || sh <= 0) {
		for (y = 0; y < h; y++)
			memset(dst + (size_t)y * pitch, 0, (size_t)w * 4);
		return;
	}
	layout(sw, sh, w, h, &ox, &oy, &dw, &dh);
	for (y = 0; y < oy; y++)
		memset(dst + (size_t)y * pitch, 0, (size_t)w * 4);
	for (y = oy + dh; y < h; y++)
		memset(dst + (size_t)y * pitch, 0, (size_t)w * 4);
	if (dw == sw * 2 && dh == sh * 2) {
		/* the usual case on the Mini: 320x240 doubled */
		for (y = 0; y < sh; y++) {
			const uint8_t *s = pic_ + (size_t)y * sw;
			uint32_t *d = dst + (size_t)(oy + 2 * y) * pitch;
			memset(d, 0, (size_t)ox * 4);
			for (x = 0; x < sw; x++) {
				uint32_t c = pal[s[x] & mask];
				d[ox + 2 * x] = c;
				d[ox + 2 * x + 1] = c;
			}
			memset(d + ox + dw, 0, (size_t)(w - ox - dw) * 4);
			memcpy(d + pitch, d, (size_t)w * 4);
		}
		return;
	}
	if (xmap_sw != sw || xmap_dw != dw) {
		for (x = 0; x < dw && x < 2048; x++)
			xmap[x] = x * sw / dw;
		xmap_sw = sw;
		xmap_dw = dw;
	}
	for (y = 0; y < dh; y++) {
		const uint8_t *s = pic_ + (size_t)(y * sh / dh) * sw;
		uint32_t *d = dst + (size_t)(oy + y) * pitch;
		memset(d, 0, (size_t)ox * 4);
		if (dw == sw) {
			for (x = 0; x < dw; x++)
				d[ox + x] = pal[s[x] & mask];
		} else {
			for (x = 0; x < dw; x++)
				d[ox + x] = pal[s[xmap[x]] & mask];
		}
		memset(d + ox + dw, 0, (size_t)(w - ox - dw) * 4);
	}
}

/* scales the current picture onto dst; call vga_update_picture() first */
void vga_render(uint32_t *dst, int w, int h, int pitch)
{
	static Picture cur;
	cur.pic = pic;
	cur.sw = pic_w;
	cur.sh = pic_h;
	cur.mask = vga.dac_mask;
	memcpy(cur.pal, vga.pal, sizeof(cur.pal));
	vga.frames++;
	vga_render_picture(&cur, dst, w, h, pitch);
}

void vga_init(void)
{
	port_register(0x3B4, 0x3BA, vga_in, vga_out);
	port_register(0x3C0, 0x3CF, vga_in, vga_out);
	port_register(0x3D4, 0x3DA, vga_in, vga_out);
	/* VESA mode list and OEM string in the video ROM */
	{
		static const uint16_t modes[] = { 0x100, 0x101, 0x103, 0x105, 0xFFFF };
		static const char oem[] = "Scorched eMiYoolator VBE";
		unsigned i;
		for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
			wr16(LIN(VBIOS_SEG, VBE_MODES_OFF + i * 2), modes[i]);
		memcpy(mem + LIN(VBIOS_SEG, VBE_OEM_OFF), oem, sizeof(oem));
	}
	set_mode(3, 1);
}
