/*
 * Miyoo Mini backend (from the GTA1 port's platform_miyoo.c).
 *
 * Video: renders into a heap buffer and copies it, rotated by 180 degrees
 * (the panel is mounted upside down), into the hidden page of a double
 * buffered /dev/fb0, then pans to it. The panel's own size is used, so the
 * Mini v4 (752x560) gets its full resolution.
 *
 * Buttons: read straight from evdev (/dev/input/event0), not grabbed, so
 * OnionOS keeps its MENU/POWER/volume handling.
 *
 * Audio: the SigmaStar MI_AO driver directly (the launcher stops Onion's
 * audioserver), at Onion's logarithmic volume setting and mute; falls back
 * to the firmware's SDL 1.2. Both are loaded at run time.
 *
 * Environment:
 *   SCORCH_FAKEFB=WxH:file   stand-in framebuffer file (for emulated tests)
 *   SCORCH_INPUT=dev[:dev]   evdev devices (default /dev/input/event0)
 */
#include "platform.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define MAX_INPUTS 4

static Framebuffer fb;
static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_size;
static struct fb_var_screeninfo var, orig_var;
static struct fb_fix_screeninfo fix;
static int pages = 1;
static int page;
static int fake, headless;

static int in_fd[MAX_INPUTS];
static int in_count;
static uint32_t buttons;
static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static int open_fake(const char *spec)
{
	const char *path = strchr(spec, ':');
	unsigned w, h;

	if (path == NULL || sscanf(spec, "%ux%u", &w, &h) != 2)
		return -1;
	fb_fd = open(path + 1, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fb_fd < 0)
		return -1;
	memset(&var, 0, sizeof(var));
	memset(&fix, 0, sizeof(fix));
	var.xres = var.xres_virtual = w;
	var.yres = var.yres_virtual = h;
	var.bits_per_pixel = 32;
	fix.line_length = w * 4;
	fix.smem_len = w * h * 4;
	if (ftruncate(fb_fd, fix.smem_len) != 0)
		return -1;
	fake = 1;
	return 0;
}

static int open_fb(void)
{
	const char *fakespec = getenv("SCORCH_FAKEFB");

	if (fakespec != NULL && fakespec[0] != 0)
		return open_fake(fakespec);

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		fprintf(stderr, "fb0: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0)
		return -1;
	orig_var = var;

	/* ask for 32bpp with two pages to flip between */
	var.bits_per_pixel = 32;
	var.yres_virtual = var.yres * 2;
	var.yoffset = 0;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) < 0) {
		var = orig_var;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0)
		return -1;
	if (var.bits_per_pixel != 32) {
		fprintf(stderr, "fb0: unsupported depth %u\n", var.bits_per_pixel);
		return -1;
	}
	return 0;
}

static void open_inputs(void)
{
	const char *env = getenv("SCORCH_INPUT");
	char *list = strdup(env != NULL && env[0] ? env : "/dev/input/event0");
	char *save = NULL, *dev;

	for (dev = strtok_r(list, ":", &save); dev != NULL && in_count < MAX_INPUTS;
	     dev = strtok_r(NULL, ":", &save)) {
		int fd = open(dev, O_RDONLY | O_NONBLOCK);
		if (fd >= 0)
			in_fd[in_count++] = fd;
		else
			fprintf(stderr, "input %s: %s\n", dev, strerror(errno));
	}
	free(list);
}

int plat_init(int hl, int pad_mode)
{
	struct sigaction sa;

	(void)pad_mode;
	headless = hl;
	if (headless) {
		fb.w = 640;
		fb.h = 480;
		fb.pitch = 640;
		fb.pixels = calloc((size_t)fb.w * fb.h, 4);
		return fb.pixels ? 0 : -1;
	}
	if (open_fb() != 0)
		return -1;

	fb_size = fix.smem_len;
	fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fb_mem = NULL;
		return -1;
	}
	pages = (!fake && var.yres_virtual >= var.yres * 2 &&
	         fb_size >= (size_t)fix.line_length * var.yres * 2) ? 2 : 1;

	fb.w = var.xres;
	fb.h = var.yres;
	fb.pitch = fb.w;
	fb.pixels = calloc((size_t)fb.w * fb.h, 4);
	if (fb.pixels == NULL)
		return -1;

	open_inputs();

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	return 0;
}

void plat_shutdown(void)
{
	int i;

	plat_audio_close();
	if (fb_mem != NULL) {
		if (!fake) {
			memset(fb_mem, 0, fb_size);
			var.yoffset = 0;
			ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
			ioctl(fb_fd, FBIOPUT_VSCREENINFO, &orig_var);
		}
		munmap(fb_mem, fb_size);
		fb_mem = NULL;
	}
	if (fb_fd >= 0)
		close(fb_fd);
	fb_fd = -1;
	for (i = 0; i < in_count; i++)
		close(in_fd[i]);
	in_count = 0;
	free(fb.pixels);
	fb.pixels = NULL;
}

Framebuffer *plat_framebuffer(void)
{
	return &fb;
}

void plat_present(void)
{
	int target = pages == 2 ? page ^ 1 : 0;
	uint8_t *base;
	int y;

	if (headless || fb_mem == NULL)
		return;
	base = fb_mem + (size_t)target * var.yres * fix.line_length;
	/* 180 degree rotation: last source pixel goes to the first panel pixel */
	for (y = 0; y < fb.h; y++) {
		const uint32_t *src = fb.pixels + (size_t)(fb.h - 1 - y) * fb.pitch + fb.w - 1;
		uint32_t *dst = (uint32_t *)(base + (size_t)y * fix.line_length);
		int x;
		for (x = 0; x < fb.w; x++)
			dst[x] = *src-- | 0xFF000000u;
	}

	if (pages == 2) {
		var.yoffset = target * var.yres;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
		page = target;
	}
}

static int map_key(int code)
{
	switch (code) {
	case KEY_UP: return PAD_UP;
	case KEY_DOWN: return PAD_DOWN;
	case KEY_LEFT: return PAD_LEFT;
	case KEY_RIGHT: return PAD_RIGHT;
	case KEY_SPACE: return PAD_A;
	case KEY_LEFTCTRL: return PAD_B;
	case KEY_LEFTSHIFT: return PAD_X;
	case KEY_LEFTALT: return PAD_Y;
	case KEY_E: return PAD_L1;
	case KEY_T: return PAD_R1;
	case KEY_TAB: return PAD_L2;
	case KEY_BACKSPACE: return PAD_R2;
	case KEY_RIGHTCTRL: return PAD_SELECT;
	case KEY_ENTER: return PAD_START;
	case KEY_ESC: return PAD_MENU;
	default: return -1;
	}
}

uint32_t plat_poll(const PlatInput *in)
{
	struct input_event ev[16];
	int i;

	(void)in;
	for (i = 0; i < in_count; i++) {
		ssize_t n;
		while ((n = read(in_fd[i], ev, sizeof(ev))) > 0) {
			int k, count = (int)(n / sizeof(ev[0]));
			for (k = 0; k < count; k++) {
				int b;
				if (ev[k].type != EV_KEY)
					continue;
				b = map_key(ev[k].code);
				if (b < 0)
					continue;
				if (ev[k].value) /* 1 press, 2 repeat */
					buttons |= PAD_BIT(b);
				else
					buttons &= ~PAD_BIT(b);
			}
		}
	}
	return buttons;
}

int plat_quit_requested(void)
{
	return quit;
}

/* ---- audio -----------------------------------------------------------------
 *
 * First choice: the SigmaStar MI_AO driver directly (what the firmware's own
 * players use), fed by a mixer thread; the launch script stops OnionOS's
 * audioserver so the device is free. Fallback: the firmware's SDL 1.2.
 */

#include <pthread.h>
#include <mi_ao.h>
#include <mi_sys.h>

#define AO_DEV 0
#define AO_CHN 0
#define AO_FRAMES 512

static AudioCallback audio_cb;
static void *audio_user;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *ao_lib, *sys_lib;
static pthread_t ao_thread;
static volatile int ao_running;
static MI_S32 (*p_MI_SYS_Init)(void);
static MI_S32 (*p_MI_AO_SetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *);
static MI_S32 (*p_MI_AO_Enable)(MI_AUDIO_DEV);
static MI_S32 (*p_MI_AO_Disable)(MI_AUDIO_DEV);
static MI_S32 (*p_MI_AO_EnableChn)(MI_AUDIO_DEV, MI_AO_CHN);
static MI_S32 (*p_MI_AO_DisableChn)(MI_AUDIO_DEV, MI_AO_CHN);
static MI_S32 (*p_MI_AO_SendFrame)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32);
static MI_S32 (*p_MI_AO_SetVolume)(MI_AUDIO_DEV, MI_S32);
static MI_S32 (*p_MI_AO_SetMute)(MI_AUDIO_DEV, MI_BOOL);

/* Onion's logarithmic volume curve (src/common/system/volume.h in OnionUI) */
static int volume_db(int vol)
{
	if (vol < 0)
		vol = 0;
	if (vol > 20)
		vol = 20;
	return vol == 0 ? -60 : (int)lround(48.0 * log10(1.0 + vol)) - 60;
}

/* Onion keeps the volume (0..20) on the card; /appconfigs is the firmware's */
static int system_volume_db(void)
{
	char buf[2048];
	FILE *f = fopen("/mnt/SDCARD/system.json", "r");
	int vol = 10;

	if (f == NULL)
		f = fopen("/appconfigs/system.json", "r");
	if (f != NULL) {
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		char *v;
		buf[n] = 0;
		fclose(f);
		v = strstr(buf, "\"vol\"");
		if (v != NULL && (v = strchr(v, ':')) != NULL)
			vol = atoi(v + 1);
	}
	return volume_db(vol);
}

static void *ao_loop(void *arg)
{
	static int16_t buf[AO_FRAMES * 2];
	MI_AUDIO_Frame_t frame;

	(void)arg;
	while (ao_running) {
		pthread_mutex_lock(&audio_mutex);
		audio_cb(buf, AO_FRAMES, audio_user);
		pthread_mutex_unlock(&audio_mutex);
		memset(&frame, 0, sizeof(frame));
		frame.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
		frame.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
		frame.u32Len = sizeof(buf);
		frame.apVirAddr[0] = buf;
		/* blocks while the driver's queue is full, which paces the thread */
		while (ao_running && p_MI_AO_SendFrame(AO_DEV, AO_CHN, &frame, 100) != 0)
			usleep(2000);
	}
	return NULL;
}

static void *load_lib(const char *name)
{
	char path[128];
	void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
	if (h == NULL) {
		snprintf(path, sizeof(path), "/config/lib/%s", name);
		h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	}
	return h;
}

static int mi_ao_open(int rate)
{
	MI_AUDIO_Attr_t attr;
	MI_S32 vol;
	int mute;

	/* libmi_ao.so uses symbols of SigmaStar's OS wrapper without linking it */
	load_lib("libcam_os_wrapper.so");
	load_lib("libmi_common.so");
	sys_lib = load_lib("libmi_sys.so");
	ao_lib = load_lib("libmi_ao.so");
	if (sys_lib == NULL || ao_lib == NULL) {
		fprintf(stderr, "audio: MI_AO unavailable: %s\n", dlerror());
		return -1;
	}
	p_MI_SYS_Init = (MI_S32 (*)(void))dlsym(sys_lib, "MI_SYS_Init");
	p_MI_AO_SetPubAttr = (MI_S32 (*)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *))dlsym(ao_lib, "MI_AO_SetPubAttr");
	p_MI_AO_Enable = (MI_S32 (*)(MI_AUDIO_DEV))dlsym(ao_lib, "MI_AO_Enable");
	p_MI_AO_Disable = (MI_S32 (*)(MI_AUDIO_DEV))dlsym(ao_lib, "MI_AO_Disable");
	p_MI_AO_EnableChn = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN))dlsym(ao_lib, "MI_AO_EnableChn");
	p_MI_AO_DisableChn = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN))dlsym(ao_lib, "MI_AO_DisableChn");
	p_MI_AO_SendFrame = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32))dlsym(ao_lib, "MI_AO_SendFrame");
	p_MI_AO_SetVolume = (MI_S32 (*)(MI_AUDIO_DEV, MI_S32))dlsym(ao_lib, "MI_AO_SetVolume");
	p_MI_AO_SetMute = (MI_S32 (*)(MI_AUDIO_DEV, MI_BOOL))dlsym(ao_lib, "MI_AO_SetMute");
	if (!p_MI_SYS_Init || !p_MI_AO_SetPubAttr || !p_MI_AO_Enable || !p_MI_AO_EnableChn ||
	    !p_MI_AO_SendFrame) {
		fprintf(stderr, "audio: MI_AO library lacks the functions it needs\n");
		return -1;
	}

	p_MI_SYS_Init();
	memset(&attr, 0, sizeof(attr));
	attr.eSamplerate = (MI_AUDIO_SampleRate_e)rate;
	attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
	attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
	attr.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
	attr.u32FrmNum = 6;
	attr.u32PtNumPerFrm = AO_FRAMES;
	attr.u32ChnCnt = 2;
	{
		MI_S32 e1 = p_MI_AO_SetPubAttr(AO_DEV, &attr);
		MI_S32 e2 = e1 == 0 ? p_MI_AO_Enable(AO_DEV) : -1;
		MI_S32 e3 = e2 == 0 ? p_MI_AO_EnableChn(AO_DEV, AO_CHN) : -1;
		if (e1 != 0 || e2 != 0 || e3 != 0) {
			/* most often the audioserver still owns the device */
			fprintf(stderr, "audio: MI_AO setup failed (attr %#x, enable %#x, channel %#x)\n",
			        (unsigned)e1, (unsigned)e2, (unsigned)e3);
			return -1;
		}
	}
	vol = system_volume_db();
	mute = vol <= -60 || access("/mnt/SDCARD/.tmp_update/config/.muteVolume", F_OK) == 0;
	if (p_MI_AO_SetVolume)
		p_MI_AO_SetVolume(AO_DEV, vol);
	if (p_MI_AO_SetMute)
		p_MI_AO_SetMute(AO_DEV, mute ? TRUE : FALSE);
	ao_running = 1;
	if (pthread_create(&ao_thread, NULL, ao_loop, NULL) != 0) {
		ao_running = 0;
		return -1;
	}
	fprintf(stderr, "audio: MI_AO at %d Hz, volume %d dB%s\n", rate, (int)vol, mute ? ", muted" : "");
	return 0;
}

static void mi_ao_close(void)
{
	if (!ao_running)
		return;
	ao_running = 0;
	pthread_join(ao_thread, NULL);
	if (p_MI_AO_DisableChn)
		p_MI_AO_DisableChn(AO_DEV, AO_CHN);
	if (p_MI_AO_Disable)
		p_MI_AO_Disable(AO_DEV);
}

/* ---- fallback: the firmware's SDL 1.2 ---- */

typedef struct {
	int freq;
	uint16_t format;
	uint8_t channels;
	uint8_t silence;
	uint16_t samples;
	uint16_t padding;
	uint32_t size;
	void (*callback)(void *userdata, uint8_t *stream, int len);
	void *userdata;
} SDL12_AudioSpec;

#define SDL12_INIT_AUDIO 0x10
#define SDL12_AUDIO_S16LSB 0x8010

static void *sdl_lib;
static int (*p_SDL_Init)(uint32_t);
static int (*p_SDL_OpenAudio)(SDL12_AudioSpec *, SDL12_AudioSpec *);
static void (*p_SDL_PauseAudio)(int);
static void (*p_SDL_CloseAudio)(void);
static void (*p_SDL_QuitSubSystem)(uint32_t);
static int sdl_open;

static void sdl12_audio(void *user, uint8_t *stream, int len)
{
	(void)user;
	pthread_mutex_lock(&audio_mutex);
	audio_cb((int16_t *)stream, len / 4, audio_user);
	pthread_mutex_unlock(&audio_mutex);
}

static int sdl12_open(int rate)
{
	SDL12_AudioSpec want;

	sdl_lib = dlopen("libSDL-1.2.so.0", RTLD_NOW);
	if (sdl_lib == NULL)
		return -1;
	p_SDL_Init = (int (*)(uint32_t))dlsym(sdl_lib, "SDL_Init");
	p_SDL_OpenAudio = (int (*)(SDL12_AudioSpec *, SDL12_AudioSpec *))dlsym(sdl_lib, "SDL_OpenAudio");
	p_SDL_PauseAudio = (void (*)(int))dlsym(sdl_lib, "SDL_PauseAudio");
	p_SDL_CloseAudio = (void (*)(void))dlsym(sdl_lib, "SDL_CloseAudio");
	p_SDL_QuitSubSystem = (void (*)(uint32_t))dlsym(sdl_lib, "SDL_QuitSubSystem");
	if (!p_SDL_Init || !p_SDL_OpenAudio || !p_SDL_PauseAudio || !p_SDL_CloseAudio)
		return -1;
	if (p_SDL_Init(SDL12_INIT_AUDIO) != 0)
		return -1;
	memset(&want, 0, sizeof(want));
	want.freq = rate;
	want.format = SDL12_AUDIO_S16LSB;
	want.channels = 2;
	want.samples = 1024;
	want.callback = sdl12_audio;
	if (p_SDL_OpenAudio(&want, NULL) != 0)
		return -1;
	sdl_open = 1;
	p_SDL_PauseAudio(0);
	fprintf(stderr, "audio: SDL 1.2 at %d Hz\n", rate);
	return 0;
}

int plat_audio_open(int rate, AudioCallback cb, void *user)
{
	audio_cb = cb;
	audio_user = user;
	if (headless || getenv("SCORCH_FAKEFB") != NULL)
		return -1; /* emulated tests have no audio hardware */
	if (mi_ao_open(rate) == 0)
		return 0;
	if (sdl12_open(rate) == 0)
		return 0;
	fprintf(stderr, "audio: no output available\n");
	return -1;
}

void plat_audio_close(void)
{
	mi_ao_close();
	if (sdl_open) {
		p_SDL_CloseAudio();
		if (p_SDL_QuitSubSystem)
			p_SDL_QuitSubSystem(SDL12_INIT_AUDIO);
		sdl_open = 0;
	}
}
