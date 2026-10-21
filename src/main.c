/*
 * Scorched Earth eMiYoolator: the original SCORCH.EXE (v1.5), statically
 * recompiled to native code, on a tiny emulated DOS PC.
 *
 *   scorch [options]
 *     --dir DIR        the game folder (SCORCH.EXE, *.MTN, TALK*.CFG); default "."
 *     --exe FILE       the executable (default DIR/SCORCH.EXE)
 *     --args "..."     DOS command line (e.g. a config file name)
 *     --headless       no window (tests); implies --virtual
 *     --virtual        deterministic virtual time (25 MIPS) instead of the clock
 *     --mips N         virtual CPU speed
 *     --ms N           stop after N ms of emulated time
 *     --script FILE    timed input (see script.c)
 *     --shot-at MS:FILE  screenshot at a time (repeatable)
 *     --pad            desktop: the keyboard acts as the Miyoo's buttons
 *     --interp         interpreter only (no compiled code)
 *     --trace          log every interpreted instruction
 *     --log FILE       log file (default stderr)
 *     --entries-out F  write the entry points the interpreter ran to F
 */
#include "machine.h"
#include "platform.h"
#include "ui.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUDIO_RATE 22050

static int headless, pad_mode;
static const char *entries_out;
static const char *dump_mem;
static uint64_t frames;
static char exe_path[1024];

void script_load(const char *path);
void script_poll(uint64_t now_ms);
void script_shot_at(uint64_t ms, const char *file);
uint32_t script_pad_state(void);
long long machine_interp_count(void);

/* ---- input from the desktop ----------------------------------------------------------- */

static void on_key(int xt, int down)
{
	kbd_key(xt, down);
}

static void on_mouse_abs(int x, int y)
{
	ui_pointer_from_screen(x, y);
}

static void on_mouse_buttons(int mask)
{
	mouse_buttons(mask);
}

static const PlatInput input_hooks = { on_key, on_mouse_abs, on_mouse_buttons };

void frontend_poll(void)
{
	static uint64_t menu_since;
	uint32_t pad = plat_poll(&input_hooks) | script_pad_state();
	if (plat_quit_requested()) {
		LOG("quit requested");
		machine_exit(0);
	}
	/* holding MENU for two seconds always leaves the game */
	if (pad & PAD_BIT(PAD_MENU)) {
		if (!menu_since)
			menu_since = time_us() + 1;
		else if (time_us() - menu_since > 2000000) {
			LOG("MENU held: quitting");
			machine_exit(0);
		}
	} else {
		menu_since = 0;
	}
	ui_update(pad, time_us());
	script_poll(time_us() / 1000);
}

/* ---- frames ------------------------------------------------------------------------------ */

void write_ppm(const char *path)
{
	Framebuffer *fb = plat_framebuffer();
	FILE *f = fopen(path, "wb");
	int x, y;
	if (!f) {
		LOG("cannot write %s", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", fb->w, fb->h);
	for (y = 0; y < fb->h; y++) {
		for (x = 0; x < fb->w; x++) {
			uint32_t p = fb->pixels[y * fb->pitch + x];
			fputc((p >> 16) & 255, f);
			fputc((p >> 8) & 255, f);
			fputc(p & 255, f);
		}
	}
	fclose(f);
	LOG("screenshot %s", path);
}

/*
 * Frames. On the Mini a second thread (the SSD202D has two cores) scales,
 * draws the overlay and flips the page, so the emulation never waits for
 * the display: frontend_frame() only copies the picture when it changed.
 * Elsewhere (desktop, headless tests) rendering stays on this thread.
 */
#include <pthread.h>

static int render_threaded;
static pthread_t render_thread;
static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t render_cond = PTHREAD_COND_INITIALIZER;
static Picture shared_pic, render_pic;
static int render_request, render_quit;
static unsigned shared_seq, render_seq;

static void *render_main(void *arg)
{
	Framebuffer *fb = plat_framebuffer();
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&render_mutex);
		while (!render_request && !render_quit)
			pthread_cond_wait(&render_cond, &render_mutex);
		if (render_quit) {
			pthread_mutex_unlock(&render_mutex);
			break;
		}
		render_request = 0;
		if (render_seq != shared_seq) {
			uint8_t *keep = render_pic.pic;
			render_pic = shared_pic;
			render_pic.pic = keep;
			if (shared_pic.sw > 0 && shared_pic.sh > 0)
				memcpy(render_pic.pic, shared_pic.pic, (size_t)shared_pic.sw * shared_pic.sh);
			render_seq = shared_seq;
		}
		pthread_mutex_unlock(&render_mutex);
		vga_render_picture(&render_pic, fb->pixels, fb->w, fb->h, fb->pitch);
		ui_draw(fb);
		plat_present();
	}
	return NULL;
}

static __attribute__((unused)) void render_start(void)
{
	shared_pic.pic = calloc(PIC_MAX, 1);
	render_pic.pic = calloc(PIC_MAX, 1);
	if (!shared_pic.pic || !render_pic.pic)
		return;
	if (pthread_create(&render_thread, NULL, render_main, NULL) == 0)
		render_threaded = 1;
}

static void render_stop(void)
{
	if (!render_threaded)
		return;
	pthread_mutex_lock(&render_mutex);
	render_quit = 1;
	pthread_cond_signal(&render_cond);
	pthread_mutex_unlock(&render_mutex);
	pthread_join(render_thread, NULL);
	render_threaded = 0;
}

void frontend_frame(void)
{
	Framebuffer *fb = plat_framebuffer();
	int changed = vga_present_due(time_us()), uidirty = ui_needs_redraw();

	frames++;
	if (!changed && !uidirty)
		return;
	if (!render_threaded) {
		vga_render(fb->pixels, fb->w, fb->h, fb->pitch);
		ui_draw(fb);
		plat_present();
		return;
	}
	pthread_mutex_lock(&render_mutex);
	if (changed) {
		vga_snapshot(&shared_pic);
		shared_seq++;
	}
	render_request = 1;
	pthread_cond_signal(&render_cond);
	pthread_mutex_unlock(&render_mutex);
}

/* ---- exit ------------------------------------------------------------------------------- */

void machine_exit(int code)
{
	static int exiting;
	if (exiting)
		_exit(code);
	exiting = 1;
	LOG("exit %d after %llu instructions (%lld interpreted), %.1f s emulated, %llu frames, at %04x:%04x",
	    code, (unsigned long long)cpu.icount, machine_interp_count(), time_us() / 1e6,
	    (unsigned long long)frames, sCS, cpu.ip);
	if (mcfg.realtime && time_us() > 0) {
		extern uint64_t slept_us, sleeps, sleep_max_us;
		double secs = time_us() / 1e6, busy = secs - slept_us / 1e6;
		extern uint64_t irq0_count, pit_dropped;
		LOG("speed: %.1f M instructions/s while busy, idle %.0f%% of the time (%llu sleeps, %.0f us each, "
		    "longest %llu), timer %.0f/s (%llu ticks dropped)",
		    busy > 0 ? cpu.icount / busy / 1e6 : 0.0, 100.0 * slept_us / 1e6 / secs,
		    (unsigned long long)sleeps, sleeps ? (double)slept_us / sleeps : 0.0,
		    (unsigned long long)sleep_max_us, irq0_count / secs, (unsigned long long)pit_dropped);
	}
	if (dump_mem) {
		FILE *f = fopen(dump_mem, "wb");
		if (f) {
			fwrite(mem, 1, MEM_SIZE, f);
			fclose(f);
		}
	}
	if (entries_out) {
		uncompiled_write(entries_out);
	} else if (!mcfg.no_compiled && compiled_image_sum) {
		char path[1100];
		snprintf(path, sizeof(path), "%s/uncompiled.txt", mcfg.game_dir);
		uncompiled_write(path);
	}
	dos_shutdown();
	render_stop();
	plat_shutdown();
	if (log_file)
		fclose(log_file);
	exit(code);
}

static void on_signal(int sig)
{
	(void)sig;
	machine_exit(130);
}

/* ---- audio ------------------------------------------------------------------------------- */

static void audio_cb(int16_t *out, int frames_, void *user)
{
	(void)user;
	speaker_render(out, frames_, AUDIO_RATE);
}

/* ---- main ------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *dir = ".", *exe = NULL, *script = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--dir") && v) {
			dir = v;
			i++;
		} else if (!strcmp(a, "--exe") && v) {
			exe = v;
			i++;
		} else if (!strcmp(a, "--args") && v) {
			mcfg.args = v;
			i++;
		} else if (!strcmp(a, "--headless")) {
			headless = 1;
			if (mcfg.realtime != 2)
				mcfg.realtime = 0;
		} else if (!strcmp(a, "--virtual")) {
			mcfg.realtime = 0;
		} else if (!strcmp(a, "--mips") && v) {
			mcfg.mips = atof(v);
			i++;
		} else if (!strcmp(a, "--ms") && v) {
			mcfg.max_ms = strtoull(v, NULL, 10);
			i++;
		} else if (!strcmp(a, "--script") && v) {
			script = v;
			i++;
		} else if (!strcmp(a, "--shot-at") && v) {
			const char *colon = strchr(v, ':');
			if (colon)
				script_shot_at(strtoull(v, NULL, 10), colon + 1);
			i++;
		} else if (!strcmp(a, "--realtime")) {
			mcfg.realtime = 2; /* even when headless */
		} else if (!strcmp(a, "--fps") && v) {
			mcfg.fps = atoi(v);
			i++;
		} else if (!strcmp(a, "--pad")) {
			pad_mode = 1;
		} else if (!strcmp(a, "--interp")) {
			mcfg.no_compiled = 1;
		} else if (!strcmp(a, "--trace")) {
			mcfg.trace = 1;
		} else if (!strcmp(a, "--trace-cs") && v) {
			mcfg.trace = (int)strtol(v, NULL, 16);
			i++;
		} else if (!strcmp(a, "--trace-events") && v) {
			extern FILE *event_trace;
			event_trace = fopen(v, "w");
			i++;
		} else if (!strcmp(a, "--dump-mem") && v) {
			dump_mem = v;
			i++;
		} else if (!strcmp(a, "--entries-out") && v) {
			entries_out = v;
			i++;
		} else if (!strcmp(a, "--log-io")) {
			mcfg.log_io = 1;
		} else if (!strcmp(a, "--log") && v) {
			log_file = fopen(v, "w");
			i++;
		} else {
			fprintf(stderr, "unknown option %s\n", a);
			return 2;
		}
	}
	mcfg.game_dir = dir;
#ifdef PLATFORM_MIYOO
	if (!mcfg.fps)
		mcfg.fps = 30; /* enough for Scorch, and half the copying */
#endif
	if (!exe) {
		snprintf(exe_path, sizeof(exe_path), "%s/SCORCH.EXE", dir);
		if (access(exe_path, R_OK) != 0)
			snprintf(exe_path, sizeof(exe_path), "%s/scorch.exe", dir);
		exe = exe_path;
	}
	mcfg.exe = exe;
	mcfg.headless = headless;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (plat_init(headless, pad_mode) != 0) {
		LOG("cannot open the screen");
		return 1;
	}
	machine_init();
	compiled_init();
	LOG("%s", compiled_info);
	if (dos_load(exe, mcfg.args) != 0) {
		plat_shutdown();
		return 1;
	}
	if (compiled_image_sum && dos_image_checksum() != compiled_image_sum) {
		LOG("this SCORCH.EXE (%08x) is not the one the code was compiled from (%08x): "
		    "running it in the interpreter", dos_image_checksum(), compiled_image_sum);
		compiled_disable();
	}
	ui_init(pad_mode || PLATFORM_IS_HANDHELD);
	if (script)
		script_load(script);
	if (plat_audio_open(AUDIO_RATE, audio_cb, NULL) != 0)
		LOG("no audio");
#ifdef PLATFORM_MIYOO
	if (!headless)
		render_start();
#endif
	machine_run();
	return 0;
}
