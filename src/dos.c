/*
 * DOS: the EXE loader, memory blocks (MCBs), files and console (INT 21h).
 *
 * Drive C: is the game folder. File names are matched case-insensitively
 * (the game asks for "talk1.cfg", the files are TALK1.CFG); new files get
 * upper-case names like DOS would give them.
 */
#include "machine.h"
#include "ops.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ENV_SEG 0x00F0
#define MAX_HANDLES 40
#define MAX_SEARCH 16

uint32_t dos_image_size;
static int handles[MAX_HANDLES]; /* host fds; -1 free; handles 0-4 are devices */
static uint16_t dta_seg = PSP_SEG, dta_off = 0x80;
static uint16_t alloc_strategy;
static uint8_t return_code;
static uint16_t last_error;
static char cwd[80] = ""; /* DOS current directory below the root, no leading '\' */

typedef struct {
	int used;
	char dir[1024];
	char pattern[128];
	uint8_t attr;
	char **names; /* the directory's entries, read at find_first */
	int n, pos;
} Search;
static Search searches[MAX_SEARCH];

/* ---- registers & flags of the caller ---------------------------------------- */

/* the INT frame: IP, CS, FLAGS at SS:SP */
static void set_cf(int on)
{
	uint32_t a = LIN(sSS, rSP + 4);
	uint16_t f = rd16(a);
	wr16(a, (uint16_t)(on ? f | FLAG_CF : f & ~FLAG_CF));
}

static void set_zf(int on)
{
	uint32_t a = LIN(sSS, rSP + 4);
	uint16_t f = rd16(a);
	wr16(a, (uint16_t)(on ? f | FLAG_ZF : f & ~FLAG_ZF));
}

static void fail(uint16_t err)
{
	last_error = err;
	rAX = err;
	set_cf(1);
}

static void ok(void)
{
	set_cf(0);
}

static void read_str(uint16_t seg, uint16_t off, char *out, int max)
{
	int i;
	for (i = 0; i < max - 1; i++) {
		char c = (char)rd8(LIN(seg, off + i));
		if (!c)
			break;
		out[i] = c;
	}
	out[i] = 0;
}

static void write_str(uint16_t seg, uint16_t off, const char *s)
{
	do
		wr8(LIN(seg, off++), (uint8_t)*s);
	while (*s++);
}

/* ---- memory control blocks -------------------------------------------------------- */

#define MCB_END 0xA000

static uint16_t mcb_size(uint16_t m) { return rd16(LIN(m, 3)); }
static uint16_t mcb_owner(uint16_t m) { return rd16(LIN(m, 1)); }
static int mcb_last(uint16_t m) { return rd8(LIN(m, 0)) == 'Z'; }

static void mcb_write(uint16_t m, char type, uint16_t owner, uint16_t size)
{
	wr8(LIN(m, 0), (uint8_t)type);
	wr16(LIN(m, 1), owner);
	wr16(LIN(m, 3), size);
}

static uint16_t first_mcb = ENV_SEG - 1;

static void mcb_merge_free(void)
{
	uint16_t m = first_mcb;
	while (!mcb_last(m)) {
		uint16_t n = (uint16_t)(m + mcb_size(m) + 1);
		if (mcb_owner(m) == 0 && mcb_owner(n) == 0) {
			mcb_write(m, (char)rd8(LIN(n, 0)), 0, (uint16_t)(mcb_size(m) + mcb_size(n) + 1));
			continue;
		}
		m = n;
	}
}

static void dos_alloc(void)
{
	uint16_t want = rBX, m = first_mcb, best = 0, best_size = 0, largest = 0;
	int last_fit = (alloc_strategy & 3) == 2, best_fit = (alloc_strategy & 3) == 1;

	mcb_merge_free();
	for (;;) {
		uint16_t s = mcb_size(m);
		if (mcb_owner(m) == 0) {
			if (s > largest)
				largest = s;
			if (s >= want && (!best || last_fit || (best_fit && s < best_size))) {
				best = m;
				best_size = s;
			}
		}
		if (mcb_last(m))
			break;
		m = (uint16_t)(m + s + 1);
	}
	if (!best) {
		rBX = largest;
		fail(8);
		return;
	}
	if (best_size > want) {
		char type = mcb_last(best) ? 'Z' : 'M';
		if (last_fit) {
			/* take the top end */
			uint16_t n = (uint16_t)(best + best_size - want);
			mcb_write(best, 'M', 0, (uint16_t)(best_size - want - 1));
			mcb_write(n, type, PSP_SEG, want);
			rAX = (uint16_t)(n + 1);
			ok();
			return;
		}
		mcb_write((uint16_t)(best + want + 1), type, 0, (uint16_t)(best_size - want - 1));
		mcb_write(best, 'M', PSP_SEG, want);
	} else {
		wr16(LIN(best, 1), PSP_SEG);
	}
	rAX = (uint16_t)(best + 1);
	ok();
}

static void dos_free(void)
{
	uint16_t m = (uint16_t)(sES - 1);
	if (rd8(LIN(m, 0)) != 'M' && rd8(LIN(m, 0)) != 'Z') {
		fail(9);
		return;
	}
	wr16(LIN(m, 1), 0);
	mcb_merge_free();
	ok();
}

static void dos_resize(void)
{
	uint16_t m = (uint16_t)(sES - 1), want = rBX, size;
	char type;

	if (rd8(LIN(m, 0)) != 'M' && rd8(LIN(m, 0)) != 'Z') {
		fail(9);
		return;
	}
	mcb_merge_free();
	size = mcb_size(m);
	type = (char)rd8(LIN(m, 0));
	if (want > size) {
		/* grow into a free block that follows */
		uint16_t n = (uint16_t)(m + size + 1), avail = size;
		if (type != 'Z' && mcb_owner(n) == 0)
			avail = (uint16_t)(size + 1 + mcb_size(n));
		if (want > avail) {
			rBX = avail;
			fail(8);
			return;
		}
		if (avail != size) {
			type = (char)rd8(LIN(n, 0));
			mcb_write(m, type, mcb_owner(m), avail);
			size = avail;
		}
	}
	if (want < size) {
		mcb_write((uint16_t)(m + want + 1), type, 0, (uint16_t)(size - want - 1));
		mcb_write(m, 'M', mcb_owner(m), want);
		mcb_merge_free();
	}
	/* the PSP records the end of the program's block */
	if (sES == PSP_SEG)
		wr16(LIN(PSP_SEG, 2), (uint16_t)(PSP_SEG + want));
	ok();
}

/* ---- paths ----------------------------------------------------------------------- */

/* finds `name` in `dir` ignoring case; returns 1 and the real name if found */
static int match_entry(const char *dir, const char *name, char *real, size_t realsize)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL) {
		if (strcasecmp(e->d_name, name) == 0) {
			snprintf(real, realsize, "%s", e->d_name);
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

/*
 * DOS path -> host path. Existing components keep their real names; the
 * last missing one is upper-cased (for creation). Returns 0 if a directory
 * on the way does not exist.
 */
static int host_path(const char *dospath, char *out, size_t outsize)
{
	char buf[256], *comp, *save = NULL;
	const char *p = dospath;
	char cur[1024];
	int missing = 0;

	if (p[0] && p[1] == ':')
		p += 2;
	if (*p == '\\' || *p == '/') {
		snprintf(buf, sizeof(buf), "%s", p + 1);
	} else if (cwd[0]) {
		snprintf(buf, sizeof(buf), "%s\\%s", cwd, p);
	} else {
		snprintf(buf, sizeof(buf), "%s", p);
	}
	snprintf(cur, sizeof(cur), "%s", mcfg.game_dir);
	for (comp = strtok_r(buf, "\\/", &save); comp; comp = strtok_r(NULL, "\\/", &save)) {
		char real[256];
		size_t n = strlen(cur);
		if (strcmp(comp, ".") == 0)
			continue;
		if (strcmp(comp, "..") == 0) {
			/* never above the game folder */
			char *slash = strrchr(cur, '/');
			if (slash && (size_t)(slash - cur) >= strlen(mcfg.game_dir))
				*slash = 0;
			continue;
		}
		if (missing)
			return 0;
		if (match_entry(cur, comp, real, sizeof(real))) {
			snprintf(cur + n, sizeof(cur) - n, "/%s", real);
		} else {
			char up[256];
			int i;
			for (i = 0; comp[i] && i < 255; i++)
				up[i] = (char)toupper((unsigned char)comp[i]);
			up[i] = 0;
			snprintf(cur + n, sizeof(cur) - n, "/%s", up);
			missing = 1;
		}
	}
	snprintf(out, outsize, "%s", cur);
	return 1;
}

static int new_handle(int fd)
{
	int h;
	for (h = 5; h < MAX_HANDLES; h++) {
		if (handles[h] < 0) {
			handles[h] = fd;
			return h;
		}
	}
	return -1;
}

static int valid_file(int h)
{
	return h >= 5 && h < MAX_HANDLES && handles[h] >= 0;
}

/* ---- console ------------------------------------------------------------------------- */

void vga_tty_putc(uint8_t ch);

static void con_write(const char *s, int n)
{
	static char line[256];
	static int len;
	int i;
	for (i = 0; i < n; i++) {
		char c = s[i];
		vga_tty_putc((uint8_t)c);
		if (c == '\n' || len == (int)sizeof(line) - 1) {
			line[len] = 0;
			LOG("[dos] %s", line);
			len = 0;
		} else if (c != '\r') {
			line[len++] = c;
		}
	}
}

int bios_wait_key(int peek, int extended, uint16_t *key);

/* DOS returns extended keys as 0, then the scan code on the next call */
static int con_pending;
static uint8_t con_scan;

static uint8_t con_getc(int dummy)
{
	uint16_t key;
	(void)dummy;
	if (con_pending) {
		con_pending = 0;
		return con_scan;
	}
	bios_wait_key(0, 0, &key);
	if ((key & 0xFF) == 0 || (key & 0xFF) == 0xE0) {
		con_pending = 1;
		con_scan = (uint8_t)(key >> 8);
		return 0;
	}
	return (uint8_t)key;
}

/* ---- find first / next --------------------------------------------------------------- */

static int wild_match(const char *pat, const char *name)
{
	/* DOS 8.3 matching: compare base and extension separately */
	char pb[9], pe[4], nb[9], ne[4];
	const char *dot;
	int i, j;

	memset(pb, ' ', 8); memset(pe, ' ', 3); memset(nb, ' ', 8); memset(ne, ' ', 3);
	pb[8] = pe[3] = nb[8] = ne[3] = 0;
	dot = strchr(pat, '.');
	for (i = 0, j = 0; pat[i] && pat + i != dot && j < 8; i++) {
		if (pat[i] == '*') { while (j < 8) pb[j++] = '?'; break; }
		pb[j++] = (char)toupper((unsigned char)pat[i]);
	}
	if (dot)
		for (i = 1, j = 0; dot[i] && j < 3; i++) {
			if (dot[i] == '*') { while (j < 3) pe[j++] = '?'; break; }
			pe[j++] = (char)toupper((unsigned char)dot[i]);
		}
	dot = strchr(name, '.');
	if (dot == name)
		return 0; /* no hidden/dot files */
	for (i = 0, j = 0; name[i] && name + i != dot; i++) {
		if (j >= 8)
			return 0;
		nb[j++] = (char)toupper((unsigned char)name[i]);
	}
	if (dot) {
		if (strlen(dot + 1) > 3 || strchr(dot + 1, '.'))
			return 0;
		for (i = 1, j = 0; dot[i]; i++)
			ne[j++] = (char)toupper((unsigned char)dot[i]);
	}
	for (i = 0; i < 8; i++)
		if (pb[i] != '?' && pb[i] != nb[i])
			return 0;
	for (i = 0; i < 3; i++)
		if (pe[i] != '?' && pe[i] != ne[i])
			return 0;
	return 1;
}

/* a file's date and time for DOS: its real mtime, or in deterministic runs the fixed
 * day dos_now() starts from (the game reads file dates, and copying the files for a
 * test stamps them with the time of the copy: runs minutes apart took other paths) */
static struct tm file_tm(time_t mtime)
{
	struct tm tm;
	if (mcfg.realtime) {
		localtime_r(&mtime, &tm);
	} else {
		time_t t = 802267200;
		gmtime_r(&t, &tm);
	}
	return tm;
}

static void fill_dta(const char *dir, const char *name)
{
	char path[1024];
	struct stat st;
	uint32_t a = LIN(dta_seg, dta_off);
	struct tm tm;
	char up[13];
	int i;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (stat(path, &st) != 0)
		memset(&st, 0, sizeof(st));
	tm = file_tm(st.st_mtime);
	wr8(a + 0x15, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
	wr16(a + 0x16, (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
	wr16(a + 0x18, (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
	wr32(a + 0x1A, (uint32_t)st.st_size);
	for (i = 0; name[i] && i < 12; i++)
		up[i] = (char)toupper((unsigned char)name[i]);
	up[i] = 0;
	for (i = 0; i < 13; i++)
		wr8(a + 0x1E + i, (uint8_t)up[i < (int)strlen(up) ? i : (int)strlen(up)]);
}

/* descending: any fixed order makes runs reproducible; this is the order the tests
 * were written against (tests/weapons.txt plays its Riot Bomb on a scanned rock
 * mountain, the case that reaches the 182B pixel plotters) */
static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)b, *(char *const *)a);
}

static void search_free(Search *s)
{
	int i;
	for (i = 0; i < s->n; i++)
		free(s->names[i]);
	free(s->names);
	s->names = NULL;
	s->n = s->pos = 0;
}

/* the directory's entries in its own order (as DOS lists a FAT directory), or sorted by
 * name in deterministic runs: the game chooses among the .MTN files it finds, and host
 * file systems order directories differently (the same test in /tmp played another game) */
static void search_read(Search *s)
{
	DIR *d = opendir(s->dir);
	struct dirent *e;
	int cap = 0;

	s->names = NULL;
	s->n = s->pos = 0;
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		if (s->n == cap) {
			cap = cap ? cap * 2 : 64;
			s->names = realloc(s->names, sizeof(char *) * (size_t)cap);
		}
		s->names[s->n++] = strdup(e->d_name);
	}
	closedir(d);
	if (!mcfg.realtime && s->n > 1)
		qsort(s->names, (size_t)s->n, sizeof(char *), cmp_name);
}

static void find_next_in(int id)
{
	Search *s = &searches[id];

	while (s->pos < s->n) {
		const char *name = s->names[s->pos++];
		char path[1024];
		struct stat st;
		if (!wild_match(s->pattern, name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", s->dir, name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode) && !(s->attr & 0x10))
			continue;
		fill_dta(s->dir, name);
		wr8(LIN(dta_seg, dta_off), (uint8_t)id);
		wr8(LIN(dta_seg, dta_off + 1), 0xA5);
		ok();
		return;
	}
	search_free(s);
	s->used = 0;
	fail(18);
}

static void find_first(void)
{
	char dospath[128], dir[1024], *slash;
	const char *pat;
	int id;

	read_str(sDS, rDX, dospath, sizeof(dospath));
	for (id = 0; id < MAX_SEARCH && searches[id].used; id++)
		;
	if (id == MAX_SEARCH) {
		/* recycle the oldest search */
		id = 0;
		search_free(&searches[0]);
	}
	slash = strrchr(dospath, '\\');
	if (!slash)
		slash = strrchr(dospath, '/');
	if (slash) {
		*slash = 0;
		pat = slash + 1;
		if (!host_path(dospath[0] ? dospath : "\\", dir, sizeof(dir))) {
			fail(3);
			return;
		}
	} else {
		pat = dospath[0] && dospath[1] == ':' ? dospath + 2 : dospath;
		if (!host_path(".", dir, sizeof(dir)))
			snprintf(dir, sizeof(dir), "%s", mcfg.game_dir);
	}
	searches[id].used = 1;
	snprintf(searches[id].dir, sizeof(searches[id].dir), "%s", dir);
	snprintf(searches[id].pattern, sizeof(searches[id].pattern), "%s", pat);
	searches[id].attr = rCL;
	search_read(&searches[id]);
	find_next_in(id);
	if (rAX == 18) {
		/* "no more files" from the first call is "file not found" */
		fail(2);
	}
}

static void find_next(void)
{
	int id = rd8(LIN(dta_seg, dta_off));
	if (id >= MAX_SEARCH || rd8(LIN(dta_seg, dta_off + 1)) != 0xA5 || !searches[id].used) {
		fail(18);
		return;
	}
	find_next_in(id);
}

/* ---- the program's own clock for dates ------------------------------------------- */

static struct tm dos_now(void)
{
	time_t t;
	struct tm tm;
	if (mcfg.realtime) {
		t = time(NULL);
	} else {
		/* deterministic runs: a fixed day in 1995, plus emulated time */
		t = 802267200 + (time_t)(time_us() / 1000000);
		gmtime_r(&t, &tm); /* the same on every machine */
		return tm;
	}
	localtime_r(&t, &tm);
	return tm;
}

/* ---- INT 21h ------------------------------------------------------------------------ */

void dos_int21(void)
{
	uint8_t ah = rAH;
	char name[256], path[1024];
	uint16_t key;

	switch (ah) {
	case 0x01: case 0x07: case 0x08: /* character input */
		rAL = con_getc(0);
		if (ah == 0x01 && rAL)
			con_write((char *)&rAL, 1);
		break;
	case 0x02:
		con_write((char *)&rDL, 1);
		break;
	case 0x06:
		if (rDL == 0xFF) {
			if (con_pending || bios_wait_key(1, 0, &key)) {
				rAL = con_getc(0);
				set_zf(0);
			} else {
				rAL = 0;
				set_zf(1);
			}
		} else {
			con_write((char *)&rDL, 1);
		}
		break;
	case 0x09: {
		uint16_t off = rDX;
		char c;
		int n = 0;
		while ((c = (char)rd8(LIN(sDS, off++))) != '$' && n++ < 4096)
			con_write(&c, 1);
		rAL = '$';
		break;
	}
	case 0x0B:
		rAL = con_pending || bios_wait_key(1, 0, &key) ? 0xFF : 0;
		break;
	case 0x0C: {
		/* flush the type-ahead buffer, then do function AL */
		con_pending = 0;
		while (bios_wait_key(1, 0, &key))
			bios_wait_key(0, 0, &key);
		if (rAL == 0x01 || rAL == 0x06 || rAL == 0x07 || rAL == 0x08 || rAL == 0x0A) {
			rAH = rAL;
			dos_int21();
		}
		break;
	}
	case 0x0E:
		rAL = 26;
		break;
	case 0x19:
		rAL = 2; /* C: */
		break;
	case 0x1A:
		dta_seg = sDS;
		dta_off = rDX;
		break;
	case 0x25:
		wr16((uint32_t)rAL * 4, rDX);
		wr16((uint32_t)rAL * 4 + 2, sDS);
		break;
	case 0x2A: {
		struct tm tm = dos_now();
		rCX = (uint16_t)(tm.tm_year + 1900);
		rDH = (uint8_t)(tm.tm_mon + 1);
		rDL = (uint8_t)tm.tm_mday;
		rAL = (uint8_t)tm.tm_wday;
		break;
	}
	case 0x2B: case 0x2D:
		rAL = 0;
		break;
	case 0x2C: {
		struct tm tm = dos_now();
		uint64_t us = time_us();
		rCH = (uint8_t)tm.tm_hour;
		rCL = (uint8_t)tm.tm_min;
		rDH = (uint8_t)tm.tm_sec;
		rDL = (uint8_t)((us / 10000) % 100);
		break;
	}
	case 0x2F:
		sES = dta_seg;
		rBX = dta_off;
		break;
	case 0x30:
		rAX = 0x0005; /* DOS 5.00 */
		rBX = 0;
		rCX = 0;
		break;
	case 0x33:
		if (rAL == 0)
			rDL = 0;
		else if (rAL == 5)
			rDL = 3; /* boot drive C: */
		break;
	case 0x35:
		rBX = rd16((uint32_t)rAL * 4);
		sES = rd16((uint32_t)rAL * 4 + 2);
		break;
	case 0x36: /* free disk space: plenty */
		rAX = 64;   /* sectors per cluster */
		rBX = 0x7FFF;
		rCX = 512;
		rDX = 0xFFFF;
		break;
	case 0x37:
		if (rAL == 0)
			rDL = '/';
		break;
	case 0x38: {
		uint32_t a = LIN(sDS, rDX);
		int i;
		for (i = 0; i < 34; i++)
			wr8(a + i, 0);
		wr8(a + 2, '$');
		wr8(a + 7, ',');
		wr8(a + 9, '.');
		wr8(a + 11, '-');
		wr8(a + 13, ':');
		rBX = 1;
		ok();
		break;
	}
	case 0x39: case 0x3A:
		read_str(sDS, rDX, name, sizeof(name));
		if (!host_path(name, path, sizeof(path)) ||
		    (ah == 0x39 ? mkdir(path, 0755) : rmdir(path)) != 0) {
			fail(3);
			break;
		}
		ok();
		break;
	case 0x3B: {
		char probe[1024];
		struct stat st;
		read_str(sDS, rDX, name, sizeof(name));
		if (!host_path(name, probe, sizeof(probe)) || stat(probe, &st) != 0 || !S_ISDIR(st.st_mode)) {
			fail(3);
			break;
		}
		/* store the path relative to the root */
		{
			const char *rel = probe + strlen(mcfg.game_dir);
			int i;
			while (*rel == '/')
				rel++;
			snprintf(cwd, sizeof(cwd), "%s", rel);
			for (i = 0; cwd[i]; i++)
				if (cwd[i] == '/')
					cwd[i] = '\\';
		}
		ok();
		break;
	}
	case 0x3C: case 0x5A: case 0x5B: { /* create */
		int fd, h, flags = O_RDWR | O_CREAT | O_TRUNC;
		if (ah == 0x5A) {
			/* temp file: DS:DX = path ending in '\', name appended */
			uint16_t off = rDX;
			read_str(sDS, rDX, name, sizeof(name));
			off = (uint16_t)(off + strlen(name));
			snprintf(name + strlen(name), sizeof(name) - strlen(name), "TMP%05u.$$$", (unsigned)(time_us() % 100000));
			write_str(sDS, rDX, name);
			(void)off;
		} else {
			read_str(sDS, rDX, name, sizeof(name));
		}
		if (ah == 0x5B)
			flags = O_RDWR | O_CREAT | O_EXCL;
		if (!host_path(name, path, sizeof(path))) {
			fail(3);
			break;
		}
		fd = open(path, flags, 0644);
		if (fd < 0) {
			fail(errno == EEXIST ? 80 : 5);
			break;
		}
		h = new_handle(fd);
		if (h < 0) {
			close(fd);
			fail(4);
			break;
		}
		LOG("dos: create %s -> %d", name, h);
		rAX = (uint16_t)h;
		ok();
		break;
	}
	case 0x3D: { /* open */
		int fd, h, mode = rAL & 3;
		read_str(sDS, rDX, name, sizeof(name));
		if (strcasecmp(name, "CON") == 0 || strcasecmp(name, "NUL") == 0) {
			rAX = 1;
			ok();
			break;
		}
		if (!host_path(name, path, sizeof(path))) {
			fail(3);
			break;
		}
		fd = open(path, mode == 0 ? O_RDONLY : mode == 1 ? O_WRONLY : O_RDWR);
		if (fd < 0) {
			LOG("dos: open %s failed", name);
			fail(2);
			break;
		}
		h = new_handle(fd);
		if (h < 0) {
			close(fd);
			fail(4);
			break;
		}
		LOG("dos: open %s (mode %d) -> %d", name, mode, h);
		rAX = (uint16_t)h;
		ok();
		break;
	}
	case 0x3E:
		if (valid_file(rBX)) {
			fsync(handles[rBX]); /* the card may lose power any time */
			close(handles[rBX]);
			handles[rBX] = -1;
		}
		ok();
		break;
	case 0x3F: { /* read */
		uint16_t h = rBX, n = rCX, off = rDX;
		if (h == 0) {
			/* stdin: a line from the keyboard is not needed by the game */
			rAX = 0;
			ok();
			break;
		}
		if (!valid_file(h)) {
			fail(6);
			break;
		}
		{
			uint8_t buf[4096];
			uint32_t done = 0;
			while (done < n) {
				size_t chunk = n - done > sizeof(buf) ? sizeof(buf) : n - done;
				ssize_t r = read(handles[h], buf, chunk);
				size_t i;
				if (r <= 0)
					break;
				for (i = 0; i < (size_t)r; i++)
					wr8(LIN(sDS, off + done + i), buf[i]);
				done += (uint32_t)r;
				if ((size_t)r < chunk)
					break;
			}
			rAX = (uint16_t)done;
		}
		ok();
		break;
	}
	case 0x40: { /* write */
		uint16_t h = rBX, n = rCX, off = rDX;
		if (h >= 1 && h <= 4) {
			char buf[512];
			uint32_t i, k = 0;
			for (i = 0; i < n; i++) {
				buf[k++] = (char)rd8(LIN(sDS, off + i));
				if (k == sizeof(buf)) {
					con_write(buf, (int)k);
					k = 0;
				}
			}
			con_write(buf, (int)k);
			rAX = n;
			ok();
			break;
		}
		if (!valid_file(h)) {
			fail(6);
			break;
		}
		if (n == 0) {
			off_t pos = lseek(handles[h], 0, SEEK_CUR);
			if (ftruncate(handles[h], pos) != 0) {
				fail(5);
				break;
			}
			rAX = 0;
			ok();
			break;
		}
		{
			uint8_t buf[4096];
			uint32_t done = 0;
			while (done < n) {
				uint32_t chunk = n - done > sizeof(buf) ? (uint32_t)sizeof(buf) : n - done, i;
				ssize_t w;
				for (i = 0; i < chunk; i++)
					buf[i] = rd8(LIN(sDS, off + done + i));
				w = write(handles[h], buf, chunk);
				if (w <= 0)
					break;
				done += (uint32_t)w;
			}
			rAX = (uint16_t)done;
		}
		ok();
		break;
	}
	case 0x41:
		read_str(sDS, rDX, name, sizeof(name));
		if (!host_path(name, path, sizeof(path)) || unlink(path) != 0) {
			fail(2);
			break;
		}
		LOG("dos: delete %s", name);
		ok();
		break;
	case 0x42: { /* seek */
		off_t r;
		int whence = rAL == 0 ? SEEK_SET : rAL == 1 ? SEEK_CUR : SEEK_END;
		int32_t offset = (int32_t)(((uint32_t)rCX << 16) | rDX);
		if (rBX <= 4) {
			rAX = 0;
			rDX = 0;
			ok();
			break;
		}
		if (!valid_file(rBX)) {
			fail(6);
			break;
		}
		r = lseek(handles[rBX], whence == SEEK_SET ? (off_t)(uint32_t)offset : (off_t)offset, whence);
		if (r < 0) {
			fail(25);
			break;
		}
		rAX = (uint16_t)r;
		rDX = (uint16_t)((uint32_t)r >> 16);
		ok();
		break;
	}
	case 0x43: {
		struct stat st;
		read_str(sDS, rDX, name, sizeof(name));
		if (!host_path(name, path, sizeof(path)) || stat(path, &st) != 0) {
			fail(2);
			break;
		}
		if (rAL == 0)
			rCX = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
		ok();
		break;
	}
	case 0x44: /* IOCTL */
		switch (rAL) {
		case 0x00:
			if (rBX <= 2)
				rDX = 0x80D3;
			else if (rBX == 3)
				rDX = 0x80C0;
			else if (rBX == 4)
				rDX = 0xA8C0;
			else if (valid_file(rBX))
				rDX = 0x0002;
			else {
				fail(6);
				break;
			}
			rAX = rDX;
			ok();
			break;
		case 0x01:
			ok();
			break;
		case 0x06: case 0x07:
			rAL = 0xFF;
			ok();
			break;
		case 0x08:
			rAX = 1;
			ok();
			break;
		case 0x09:
			rDX = 0;
			ok();
			break;
		default:
			fail(1);
			break;
		}
		break;
	case 0x45: { /* dup */
		int fd, h;
		if (!valid_file(rBX)) {
			if (rBX <= 4) {
				rAX = rBX;
				ok();
				break;
			}
			fail(6);
			break;
		}
		fd = dup(handles[rBX]);
		h = fd >= 0 ? new_handle(fd) : -1;
		if (h < 0) {
			if (fd >= 0)
				close(fd);
			fail(4);
			break;
		}
		rAX = (uint16_t)h;
		ok();
		break;
	}
	case 0x46:
		ok();
		break;
	case 0x47:
		write_str(sDS, rSI, cwd);
		rAX = 0x0100;
		ok();
		break;
	case 0x48: dos_alloc(); break;
	case 0x49: dos_free(); break;
	case 0x4A: dos_resize(); break;
	case 0x4C:
		return_code = rAL;
		LOG("dos: program exit, code %d", rAL);
		machine_exit(rAL);
	case 0x4D:
		rAX = return_code;
		break;
	case 0x4E: find_first(); break;
	case 0x4F: find_next(); break;
	case 0x50:
		break;
	case 0x51: case 0x62:
		rBX = PSP_SEG;
		break;
	case 0x56: {
		char to[256], path2[1024];
		read_str(sDS, rDX, name, sizeof(name));
		read_str(sES, rDI, to, sizeof(to));
		if (!host_path(name, path, sizeof(path)) || !host_path(to, path2, sizeof(path2)) ||
		    rename(path, path2) != 0) {
			fail(2);
			break;
		}
		ok();
		break;
	}
	case 0x57:
		if (!valid_file(rBX)) {
			fail(6);
			break;
		}
		if (rAL == 0) {
			struct stat st;
			struct tm tm;
			fstat(handles[rBX], &st);
			tm = file_tm(st.st_mtime);
			rCX = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
			rDX = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
		}
		ok();
		break;
	case 0x58:
		if (rAL == 0) {
			rAX = alloc_strategy;
		} else if (rAL == 1) {
			alloc_strategy = rBX;
		} else if (rAL == 2) {
			rAL = 0;
		}
		ok();
		break;
	case 0x59:
		rAX = last_error;
		rBH = 1;
		rBL = 1;
		rCH = 1;
		break;
	case 0x63: /* DBCS lead byte table: empty */
		if (rAL == 0) {
			sDS = BIOS_SEG;
			rSI = 0x0F00;
			rAL = 0;
			ok();
		} else {
			fail(1);
		}
		break;
	case 0x66:
		rBX = 437;
		rDX = 437;
		ok();
		break;
	case 0x67:
		ok();
		break;
	case 0x68: case 0x6A:
		if (valid_file(rBX))
			fsync(handles[rBX]);
		ok();
		break;
	default:
		LOG("dos: unsupported INT 21h AH=%02x AL=%02x at %04x:%04x", ah, rAL,
		    rd16(LIN(sSS, rSP + 2)), rd16(LIN(sSS, rSP)));
		fail(1);
		break;
	}
}

/* FNV-1a of the loaded program (after relocation), as tools/recomp.c computes it */
uint32_t dos_image_checksum(void)
{
	uint32_t h = 2166136261u, a;
	for (a = LOAD_SEG * 16u; a < LOAD_SEG * 16u + dos_image_size; a++)
		h = (h ^ mem[a]) * 16777619u;
	return h;
}

void dos_shutdown(void)
{
	int h;
	for (h = 5; h < MAX_HANDLES; h++) {
		if (handles[h] >= 0) {
			fsync(handles[h]);
			close(handles[h]);
			handles[h] = -1;
		}
	}
}

/* ---- loader ------------------------------------------------------------------------- */

int dos_load(const char *exe, const char *args)
{
	FILE *f = fopen(exe, "rb");
	uint8_t hdr[28];
	uint16_t last, pages, nrel, hdrpar, minalloc, ss, sp, ip, cs, relofs;
	long size, img;
	uint8_t *buf;
	uint32_t i, a;
	static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0PROMPT=$P$G\0";
	int n;

	for (i = 0; i < MAX_HANDLES; i++)
		handles[i] = i < 5 ? (int)i : -1;

	if (!f) {
		LOG("cannot open %s", exe);
		return -1;
	}
	if (fread(hdr, 1, 28, f) != 28 || hdr[0] != 'M' || hdr[1] != 'Z') {
		fclose(f);
		LOG("%s is not an MZ executable", exe);
		return -1;
	}
#define W(o) ((uint16_t)(hdr[o] | (hdr[(o) + 1] << 8)))
	last = W(2); pages = W(4); nrel = W(6); hdrpar = W(8); minalloc = W(10);
	ss = W(14); sp = W(16); ip = W(20); cs = W(22); relofs = W(24);
#undef W
	size = (long)(pages - 1) * 512 + (last ? last : 512);
	img = size - hdrpar * 16;
	if (img <= 0 || (uint32_t)img + LOAD_SEG * 16u + minalloc * 16u > 0xA0000u) {
		fclose(f);
		LOG("%s: bad image size", exe);
		return -1;
	}
	buf = malloc((size_t)img);
	fseek(f, hdrpar * 16, SEEK_SET);
	if (fread(buf, 1, (size_t)img, f) != (size_t)img) {
		fclose(f);
		free(buf);
		return -1;
	}
	memcpy(mem + LOAD_SEG * 16, buf, (size_t)img);
	free(buf);
	dos_image_size = (uint32_t)img;
	/* relocations */
	fseek(f, relofs, SEEK_SET);
	for (i = 0; i < nrel; i++) {
		uint8_t r[4];
		uint16_t off, seg, v;
		if (fread(r, 1, 4, f) != 4)
			break;
		off = (uint16_t)(r[0] | (r[1] << 8));
		seg = (uint16_t)(r[2] | (r[3] << 8));
		a = LIN(LOAD_SEG + seg, off);
		v = rd16(a);
		wr16(a, (uint16_t)(v + LOAD_SEG));
	}
	fclose(f);

	/* environment block and its MCB, then the program's MCB covering all memory */
	mcb_write(ENV_SEG - 1, 'M', PSP_SEG, 0x0F);
	/* variables (ending in a double zero), a count word, the program's path */
	memcpy(mem + LIN(ENV_SEG, 0), env, sizeof(env));
	wr16(LIN(ENV_SEG, sizeof(env)), 1);
	write_str(ENV_SEG, (uint16_t)(sizeof(env) + 2), "C:\\SCORCH.EXE");
	mcb_write(PSP_SEG - 1, 'Z', PSP_SEG, MCB_END - PSP_SEG);

	/* PSP */
	a = LIN(PSP_SEG, 0);
	wr16(a, 0x20CD);
	wr16(a + 2, MCB_END);
	wr16(a + 0x16, PSP_SEG);
	for (i = 0; i < 20; i++)
		wr8(a + 0x18 + i, (uint8_t)(i < 5 ? i : 0xFF));
	wr16(a + 0x2C, ENV_SEG);
	wr16(a + 0x32, 20);
	wr16(a + 0x34, 0x18);
	wr16(a + 0x36, PSP_SEG);
	wr8(a + 0x50, 0xCD);
	wr8(a + 0x51, 0x21);
	wr8(a + 0x52, 0xCB);
	memset(mem + a + 0x5C, ' ', 11);
	mem[a + 0x5C] = 0;
	memset(mem + a + 0x6C, ' ', 11);
	mem[a + 0x6C] = 0;
	n = args ? (int)strlen(args) : 0;
	if (n > 126)
		n = 126;
	wr8(a + 0x80, (uint8_t)(n ? n + 1 : 0));
	if (n) {
		wr8(a + 0x81, ' ');
		memcpy(mem + a + 0x82, args, (size_t)n);
		wr8(a + 0x82 + n, 0x0D);
	} else {
		wr8(a + 0x81, 0x0D);
	}

	/* registers at entry */
	sCS = (uint16_t)(LOAD_SEG + cs);
	cpu.ip = ip;
	sSS = (uint16_t)(LOAD_SEG + ss);
	rSP = sp;
	sDS = sES = PSP_SEG;
	rAX = 0;
	rBX = 0;
	rCX = 0x00FF;
	rDX = PSP_SEG;
	rSI = ip;
	rDI = sp;
	rBP = 0x091C;
	cpu.ifl = 1;
	LOG("dos: loaded %s: %ld bytes at %04x, entry %04x:%04x, stack %04x:%04x",
	    exe, img, LOAD_SEG, sCS, cpu.ip, sSS, rSP);
	return 0;
}
