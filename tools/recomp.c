/*
 * Static recompiler: the game's 16-bit DOS executable -> C.
 *
 *   recomp SCORCH.EXE OUTDIR [entries.txt ...]
 *
 * Loads the program exactly as the runtime's DOS loader does (same segment,
 * relocations applied), finds its functions, and writes one C function per
 * x86 function into OUTDIR/seg_SSSS.c, plus OUTDIR/funcs.h and
 * OUTDIR/index.c (the (CS, IP) -> function table and an image checksum).
 *
 * Finding code:
 *  - the entry point, direct call/jump targets, Borland switch tables;
 *  - far pointers in data: relocated segment words pointing at a code
 *    segment, with the offset before them;
 *  - far pointers the code builds from immediates (push seg / push off,
 *    mov [x+2],seg / mov [x],off) that point at a function prologue;
 *  - entries.txt: addresses the runtime saw being interpreted
 *    ("SSSS:OOOO" per line), e.g. targets of calls through pointers.
 *
 * Straight-line instructions become calls of the shared semantics in ops.h
 * on `static const Insn` constants (GCC folds them); branches become gotos,
 * calls become C calls, returns become C returns. Instruction counting and
 * interrupt checkpoints match the interpreter exactly (see interp.c).
 */
#include "../src/insn.h"
#include "../src/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_SIZE (MEM_SIZE + 16)

static uint8_t img[IMG_SIZE];
static uint8_t relmap[IMG_SIZE]; /* 1 at relocated words */
static uint32_t img_lo, img_hi;
static uint16_t entry_cs, entry_ip;

/* ---- loading -------------------------------------------------------------------------- */

static int load(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t h[28];
	uint16_t last, pages, nrel, hdrpar, ip, cs, relofs;
	long size, n;
	uint32_t i;

	if (!f || fread(h, 1, 28, f) != 28 || h[0] != 'M' || h[1] != 'Z') {
		fprintf(stderr, "%s: not an MZ executable\n", path);
		return -1;
	}
#define W(o) ((uint16_t)(h[o] | (h[(o) + 1] << 8)))
	last = W(2); pages = W(4); nrel = W(6); hdrpar = W(8); ip = W(20); cs = W(22); relofs = W(24);
#undef W
	size = (long)(pages - 1) * 512 + (last ? last : 512);
	n = size - hdrpar * 16;
	img_lo = LOAD_SEG * 16u;
	img_hi = img_lo + (uint32_t)n;
	fseek(f, hdrpar * 16, SEEK_SET);
	if (fread(img + img_lo, 1, (size_t)n, f) != (size_t)n)
		return -1;
	fseek(f, relofs, SEEK_SET);
	for (i = 0; i < nrel; i++) {
		uint8_t r[4];
		uint32_t a;
		uint16_t v;
		if (fread(r, 1, 4, f) != 4)
			return -1;
		a = LIN(LOAD_SEG + (r[2] | (r[3] << 8)), r[0] | (r[1] << 8));
		v = (uint16_t)(img[a] | (img[a + 1] << 8));
		v = (uint16_t)(v + LOAD_SEG);
		img[a] = (uint8_t)v;
		img[a + 1] = (uint8_t)(v >> 8);
		relmap[a] = 1;
	}
	fclose(f);
	entry_cs = (uint16_t)(LOAD_SEG + cs);
	entry_ip = ip;
	return 0;
}

static uint32_t image_checksum(void)
{
	/* FNV-1a over the loaded image; the runtime compares before using the code */
	uint32_t hsh = 2166136261u, a;
	for (a = img_lo; a < img_hi; a++)
		hsh = (hsh ^ img[a]) * 16777619u;
	return hsh;
}

static int decode_at(uint16_t cs, uint16_t ip, Insn *in)
{
	uint32_t a = LIN(cs, ip);
	if (a < img_lo || a >= img_hi) {
		memset(in, 0, sizeof(*in));
		return 0;
	}
	return x86_decode(img + a, 15, ip, in);
}

/* ---- functions ------------------------------------------------------------------------ */

typedef struct {
	uint16_t cs, ip;
	uint8_t explored;
	uint8_t from_table; /* 0 call target, 1 data pointer, 2 runtime log */
} Func;

static Func *funcs;
static int nfuncs, cap_funcs;

#define HBITS 16
static int32_t fhash[1 << HBITS];

static uint32_t hkey(uint16_t cs, uint16_t ip)
{
	uint32_t k = ((uint32_t)cs << 16) | ip;
	k ^= k >> 15;
	k *= 0x2C1B3C6Du;
	k ^= k >> 12;
	return k & ((1u << HBITS) - 1);
}

static int find_func(uint16_t cs, uint16_t ip)
{
	uint32_t h = hkey(cs, ip);
	while (fhash[h] >= 0) {
		Func *f = &funcs[fhash[h]];
		if (f->cs == cs && f->ip == ip)
			return fhash[h];
		h = (h + 1) & ((1u << HBITS) - 1);
	}
	return -1;
}

static int add_func(uint16_t cs, uint16_t ip, int why)
{
	uint32_t h;
	int i = find_func(cs, ip);
	if (i >= 0)
		return i;
	if (LIN(cs, ip) < img_lo || LIN(cs, ip) >= img_hi)
		return -1;
	if (nfuncs == cap_funcs) {
		cap_funcs = cap_funcs ? cap_funcs * 2 : 1024;
		funcs = realloc(funcs, sizeof(Func) * (size_t)cap_funcs);
	}
	funcs[nfuncs].cs = cs;
	funcs[nfuncs].ip = ip;
	funcs[nfuncs].explored = 0;
	funcs[nfuncs].from_table = (uint8_t)why;
	h = hkey(cs, ip);
	while (fhash[h] >= 0)
		h = (h + 1) & ((1u << HBITS) - 1);
	fhash[h] = nfuncs;
	return nfuncs++;
}

/* ---- one function's code ---------------------------------------------------------------- */

typedef struct {
	uint16_t ip;
	uint8_t label;
	uint8_t bad;
	Insn in;
	/* jump table targets for OP_JMP_IND (static) */
	int ntab;
	uint16_t *tab;
} Node;

static Node *nodes;
static int nnodes, cap_nodes;
static int32_t node_at[65536];
static uint16_t work[65536 * 2];

static int is_tail(const Func *f, uint16_t t)
{
	/* a jump to another function's entry is a tail call */
	return t != f->ip && find_func(f->cs, t) >= 0;
}

/* Borland switch: cmp bx,N / ja default / add bx,bx / jmp cs:[bx+table] */
static int jump_table(const Func *f, int ni, uint16_t **out)
{
	Node *n = &nodes[ni];
	const Insn *in = &n->in;
	int k, count = -1, scaled = 0;
	uint16_t *t;

	if (in->a.type != OT_MEM || in->a.seg != S_CS || in->a.ea != EA_BX)
		return -1;
	for (k = 1; k <= 24 && k <= n->ip; k++) {
		int j = node_at[(uint16_t)(n->ip - k)];
		const Insn *p;
		if (j < 0)
			continue;
		p = &nodes[j].in;
		if ((p->op == OP_ADD && p->a.type == OT_REG && p->a.reg == R_BX && p->b.type == OT_REG && p->b.reg == R_BX &&
		     p->osz == 2) ||
		    (p->op == OP_SHL && p->a.type == OT_REG && p->a.reg == R_BX && p->b.type == OT_IMM && p->b.imm == 1))
			scaled = 1;
		if (p->op == OP_CMP && p->a.type == OT_REG && p->a.reg == R_BX && p->osz == 2 && p->b.type == OT_IMM) {
			count = (int)p->b.imm + 1;
			break;
		}
	}
	if (count <= 0 || count > 512 || !scaled)
		return -1;
	t = malloc(sizeof(uint16_t) * (size_t)count);
	for (k = 0; k < count; k++) {
		uint32_t a = LIN(f->cs, (uint16_t)(in->a.disp + 2 * k));
		t[k] = (uint16_t)(img[a] | (img[a + 1] << 8));
	}
	*out = t;
	return count;
}

static void explore(Func *fn, int final)
{
	/* a copy: add_func() below may move the funcs array */
	Func self = *fn, *f = &self;
	int nw = 0, i;

	for (i = 0; i < nnodes; i++)
		free(nodes[i].tab);
	nnodes = 0;
	memset(node_at, 0xFF, sizeof(node_at));
	work[nw++] = f->ip;
	while (nw > 0) {
		uint16_t ip = work[--nw], nx;
		Node *n;
		Insn *in;
		int ni;

		if (node_at[ip] >= 0)
			continue;
		if (nnodes == cap_nodes) {
			cap_nodes = cap_nodes ? cap_nodes * 2 : 4096;
			nodes = realloc(nodes, sizeof(Node) * (size_t)cap_nodes);
		}
		ni = nnodes++;
		node_at[ip] = ni;
		n = &nodes[ni];
		memset(n, 0, sizeof(*n));
		n->ip = ip;
		in = &n->in;
		if (!decode_at(f->cs, ip, in)) {
			n->bad = 1;
			continue;
		}
		nx = (uint16_t)(ip + in->len);
#define PUSH(x) do { if (nw < (int)(sizeof(work) / sizeof(work[0]))) work[nw++] = (uint16_t)(x); } while (0)
		switch (in->op) {
		case OP_JMP:
			if (!(final && is_tail(f, (uint16_t)in->a.imm)))
				PUSH(in->a.imm);
			break;
		case OP_JCC: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: case OP_JCXZ:
			if (!(final && is_tail(f, (uint16_t)in->a.imm)))
				PUSH(in->a.imm);
			PUSH(nx);
			break;
		case OP_CALL:
			add_func(f->cs, (uint16_t)in->a.imm, 0);
			PUSH(nx);
			break;
		case OP_CALLF:
			add_func(in->a.disp, (uint16_t)in->a.imm, 0);
			PUSH(nx);
			break;
		case OP_JMPF:
			add_func(in->a.disp, (uint16_t)in->a.imm, 0);
			break;
		case OP_RET: case OP_RETF: case OP_IRET:
			break;
		case OP_JMP_IND: {
			uint16_t *tab = NULL;
			int cnt = jump_table(f, ni, &tab), k;
			n = &nodes[ni];
			if (cnt > 0) {
				n->ntab = cnt;
				n->tab = tab;
				for (k = 0; k < cnt; k++)
					if (!(final && is_tail(f, tab[k])))
						PUSH(tab[k]);
			}
			break;
		}
		case OP_JMPF_IND:
			break;
		case OP_INT:
			if (in->a.imm != 0x20)
				PUSH(nx);
			break;
		default:
			PUSH(nx);
			break;
		}
#undef PUSH
	}
}

static int cmp_node(const void *a, const void *b)
{
	const Node *x = a, *y = b;
	return (int)x->ip - (int)y->ip;
}

/* ---- far pointers in data ------------------------------------------------------------------ */

static int plausible_code(uint16_t cs, uint16_t ip)
{
	Insn in;
	int i;
	uint32_t a = LIN(cs, ip);
	if (a < img_lo || a + 2 >= img_hi)
		return 0;
	if (img[a] == 0 && img[a + 1] == 0)
		return 0;
	for (i = 0; i < 6; i++) {
		if (!decode_at(cs, ip, &in))
			return 0;
		if (in.op == OP_RET || in.op == OP_RETF || in.op == OP_JMP || in.op == OP_IRET)
			return 1;
		ip = (uint16_t)(ip + in.len);
	}
	return 1;
}

static int is_code_seg(uint16_t s)
{
	int i;
	for (i = 0; i < nfuncs; i++)
		if (funcs[i].cs == s)
			return 1;
	return 0;
}

/* the game's DGROUP (image paragraph): far pointers stored there are data, and
 * may point into a code segment no instruction names (30CD, a module inside
 * 3005, is reached only through such tables; 0.3.2 interpreted it) */
#define DGROUP_PARA 0x4F38u

static int scan_far_pointers(void)
{
	uint32_t a;
	int added = 0;
	for (a = img_lo + 2; a < img_hi; a++) {
		uint16_t seg, off;
		int in_data = a >= img_lo + DGROUP_PARA * 16u;
		if (!relmap[a])
			continue;
		seg = (uint16_t)(img[a] | (img[a + 1] << 8));
		off = (uint16_t)(img[a - 2] | (img[a - 1] << 8));
		if (!(is_code_seg(seg) || (in_data && seg >= LOAD_SEG && seg < LOAD_SEG + DGROUP_PARA)) ||
		    find_func(seg, off) >= 0)
			continue;
		/* a relocation inside a decoded instruction is an immediate, not data; we
		 * cannot tell here, so require the target to look like a function start */
		if (!plausible_code(seg, off))
			continue;
		{
			uint32_t t = LIN(seg, off);
			/* prologues: push bp / enter / push si,di / mov ax,ds */
			if (img[t] != 0x55 && img[t] != 0xC8 && img[t] != 0x56 && img[t] != 0x57 && img[t] != 0x1E &&
			    img[t] != 0x8C && img[t] != 0xB8 && img[t] != 0x60 && img[t] != 0x50 && img[t] != 0x53)
				continue;
		}
		if (add_func(seg, off, 1) == nfuncs - 1)
			added++;
	}
	return added;
}

/* push imm16, mov reg,imm16 or mov r/m16,imm16 (no 66h prefix): the immediate is the
 * instruction's last word */
static int ends_in_imm16(uint16_t cs, const Node *n)
{
	uint32_t a = LIN(cs, n->ip);
	int k = 0;
	uint8_t op;
	if (n->bad || n->in.osz != 2 || (n->in.op != OP_PUSH && n->in.op != OP_MOV))
		return 0;
	while (k < 3 && (img[a + k] == 0x26 || img[a + k] == 0x2E || img[a + k] == 0x36 || img[a + k] == 0x3E))
		k++;
	op = img[a + k];
	return op == 0x68 || (op >= 0xB8 && op <= 0xBF) || op == 0xC7;
}

/* the function prologues Borland emits (push bp / mov bp,sp; enter n,0) and the one
 * of interrupt handlers that load DGROUP (push ds / mov ax,seg / mov ds,ax) */
static int strong_prologue(uint16_t cs, uint16_t ip)
{
	uint32_t t = LIN(cs, ip);
	if (t < img_lo || t + 6 > img_hi)
		return 0;
	return (img[t] == 0x55 && img[t + 1] == 0x8B && img[t + 2] == 0xEC) ||
	       (img[t] == 0xC8 && img[t + 3] == 0x00) ||
	       (img[t] == 0x1E && img[t + 1] == 0xB8 && img[t + 4] == 0x8E && img[t + 5] == 0xD8);
}

/* far pointers the code builds from immediates: the segment is a relocated immediate
 * and the offset is the immediate of the instruction next to it, as in push seg /
 * push off (callbacks passed as arguments: the explosion's pixel plotters 182B:04A3
 * and 050E, 331D:0004) or mov es:[bx+1e],seg / mov es:[bx+1c],off (2DD4:04D3). All
 * were interpreted in 0.3.3; neighbours that are not such pointers fail the prologue
 * test. Uses the nodes explore() just built for one function of segment cs. */
static int scan_code_pointers(uint16_t cs)
{
	int k, d, added = 0;
	for (k = 0; k < nnodes; k++) {
		const Node *n = &nodes[k];
		uint32_t sa;
		uint16_t seg;
		int nb[2];
		if (!ends_in_imm16(cs, n))
			continue;
		sa = LIN(cs, (uint16_t)(n->ip + n->in.len - 2));
		if (!relmap[sa])
			continue;
		seg = (uint16_t)(img[sa] | (img[sa + 1] << 8));
		if (!(is_code_seg(seg) || (seg >= LOAD_SEG && seg < LOAD_SEG + DGROUP_PARA)))
			continue;
		/* the instructions right before and right after this one */
		nb[0] = -1;
		for (d = 1; d <= 8 && d <= n->ip; d++) {
			int j = node_at[(uint16_t)(n->ip - d)];
			if (j >= 0 && (uint16_t)(nodes[j].ip + nodes[j].in.len) == n->ip) {
				nb[0] = j;
				break;
			}
		}
		nb[1] = node_at[(uint16_t)(n->ip + n->in.len)];
		for (d = 0; d < 2; d++) {
			const Node *m;
			uint32_t oa;
			uint16_t off;
			if (nb[d] < 0)
				continue;
			m = &nodes[nb[d]];
			if (!ends_in_imm16(cs, m))
				continue;
			oa = LIN(cs, (uint16_t)(m->ip + m->in.len - 2));
			if (relmap[oa])
				continue;
			off = (uint16_t)(img[oa] | (img[oa + 1] << 8));
			if (find_func(seg, off) >= 0 || !strong_prologue(seg, off) || !plausible_code(seg, off))
				continue;
			if (add_func(seg, off, 1) == nfuncs - 1)
				added++;
		}
	}
	return added;
}

/* addresses the runtime had to interpret: new functions, or resume points
 * inside functions we already have */
typedef struct {
	uint16_t cs, ip;
	uint16_t owner_cs, owner_ip; /* the function it resumes, if not its own */
	int is_resume;
} Entry;

static Entry *ents;
static int nents;

static void load_entries(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[128];
	if (!f) {
		fprintf(stderr, "%s: cannot open\n", path);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned cs, ip;
		if (line[0] == '#' || sscanf(line, "%x:%x", &cs, &ip) != 2)
			continue;
		if (LIN(cs, ip) < img_lo || LIN(cs, ip) >= img_hi)
			continue; /* code made at run time (int86 stubs on the stack) */
		ents = realloc(ents, sizeof(Entry) * (size_t)(nents + 1));
		memset(&ents[nents], 0, sizeof(Entry));
		ents[nents].cs = (uint16_t)cs;
		ents[nents].ip = (uint16_t)ip;
		nents++;
	}
	fclose(f);
	fprintf(stderr, "%s: %d addresses\n", path, nents);
}

static int cmp_ent(const void *a, const void *b)
{
	const Entry *x = a, *y = b;
	if (x->cs != y->cs)
		return (int)x->cs - (int)y->cs;
	return (int)x->ip - (int)y->ip;
}

/* which function's final code contains (cs, ip): open hashing on the address */
#define OBITS 18
static uint32_t own_key[1 << OBITS];
static int32_t own_fn[1 << OBITS];

static uint32_t okey(uint16_t cs, uint16_t ip)
{
	return ((uint32_t)cs << 16) | ip;
}

static void own_clear(void)
{
	memset(own_key, 0, sizeof(own_key));
	memset(own_fn, 0xFF, sizeof(own_fn));
}

static int32_t *own_slot(uint32_t k)
{
	uint32_t h = (k * 0x9E3779B1u) >> (32 - OBITS);
	while (own_key[h] && own_key[h] != k)
		h = (h + 1) & ((1u << OBITS) - 1);
	own_key[h] = k;
	return &own_fn[h];
}

/* ---- C output -------------------------------------------------------------------------------- */

static const char *exec_fn(int op)
{
	switch (op) {
	case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR:
	case OP_CMP: case OP_TEST:
		return "exec_alu";
	case OP_INC: case OP_DEC: return "exec_incdec";
	case OP_NEG: return "exec_neg";
	case OP_NOT: return "exec_not";
	case OP_MUL: case OP_IMUL1: return "exec_mul";
	case OP_IMUL: return "exec_imul";
	case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: case OP_SHL: case OP_SHR: case OP_SAL: case OP_SAR:
		return "exec_shift";
	case OP_SHLD: case OP_SHRD: return "exec_shxd";
	case OP_AAD: return "exec_aad";
	case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: return "exec_bt";
	case OP_BSF: case OP_BSR: return "exec_bsx";
	case OP_MOV: return "exec_mov";
	case OP_MOVZX: case OP_MOVSX: return "exec_movx";
	case OP_XCHG: return "exec_xchg";
	case OP_LEA: return "exec_lea";
	case OP_LDS: case OP_LES: case OP_LSS: case OP_LFS: case OP_LGS: return "exec_lxs";
	case OP_PUSH: return "exec_push";
	case OP_POP: return "exec_pop";
	case OP_PUSHA: return "exec_pusha";
	case OP_POPA: return "exec_popa";
	case OP_PUSHF: return "exec_pushf";
	case OP_POPF: return "exec_popf";
	case OP_CBW: return "exec_cbw";
	case OP_CWD: return "exec_cwd";
	case OP_XLAT: return "exec_xlat";
	case OP_SETCC: return "exec_setcc";
	case OP_IN: return "exec_in";
	case OP_OUT: return "exec_out";
	case OP_ENTER: return "exec_enter";
	case OP_CLC: case OP_STC: case OP_CMC: case OP_CLD: case OP_STD: case OP_CLI: case OP_STI:
		return "exec_flagop";
	case OP_SMSW: return "exec_smsw";
	case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS: case OP_INS: case OP_OUTS:
		return "exec_string";
	default:
		return NULL;
	}
}

static void fmt_operand(const Operand *o, char *buf, size_t n)
{
	snprintf(buf, n, "{%u,%u,%u,%u,%u,0x%x,0x%x}", o->type, o->size, o->reg, o->seg, o->ea, o->disp, o->imm);
}

static void insn_const(FILE *o, const Insn *in)
{
	char a[64], b[64], c[64];
	fmt_operand(&in->a, a, sizeof(a));
	fmt_operand(&in->b, b, sizeof(b));
	fmt_operand(&in->c, c, sizeof(c));
	fprintf(o, "static const Insn I = {%u,%u,%u,0x%x,%u,%u,0x%x,%u,0x%x,0x%x,%s,%s,%s};", in->op, in->len, in->osz,
	        in->rep, in->seg, in->cc, in->flags, in->fpu_esc, in->modrm, in->fmask, a, b, c);
}

/* ---- flag liveness ------------------------------------------------------------------- */

static int cc_reads(int cc)
{
	switch (cc >> 1) {
	case 0: return FM_OF;
	case 1: return FM_CF;
	case 2: return FM_ZF;
	case 3: return FM_CF | FM_ZF;
	case 4: return FM_SF;
	case 5: return FM_PF;
	case 6: return FM_SF | FM_OF;
	default: return FM_ZF | FM_SF | FM_OF;
	}
}

/* status flags read, surely written (killed) and maybe written */
static void flag_effects(const Insn *in, int *reads, int *kills, int *may)
{
	int cnt;
	*reads = *kills = *may = 0;
	switch (in->op) {
	case OP_ADC: case OP_SBB:
		*reads = FM_CF;
		*kills = FM_ALL;
		break;
	case OP_ADD: case OP_SUB: case OP_CMP: case OP_AND: case OP_OR: case OP_XOR: case OP_TEST: case OP_NEG:
	case OP_AAM: case OP_AAD: case OP_POPF:
		*kills = FM_ALL;
		break;
	case OP_INC: case OP_DEC:
		*kills = FM_ALL & ~FM_CF;
		break;
	case OP_MUL: case OP_IMUL1: case OP_IMUL:
		*kills = FM_CF | FM_OF;
		break;
	case OP_SHL: case OP_SHR: case OP_SAL: case OP_SAR:
		if (in->b.type == OT_IMM) {
			if (in->b.imm & 0x1F)
				*kills = FM_ALL;
		} else {
			*may = FM_ALL;
		}
		break;
	case OP_RCL: case OP_RCR:
		*reads = FM_CF;
		/* fall through */
	case OP_ROL: case OP_ROR:
		cnt = in->b.type == OT_IMM ? (int)(in->b.imm & 0x1F) : -1;
		if ((in->op == OP_RCL || in->op == OP_RCR) && cnt > 0)
			cnt %= in->osz * 8 + 1;
		if (cnt > 0)
			*kills = FM_CF | FM_OF;
		else if (cnt < 0)
			*may = FM_CF | FM_OF;
		break;
	case OP_SHLD: case OP_SHRD:
		if (in->c.type == OT_IMM) {
			if (in->c.imm & 0x1F)
				*kills = FM_CF | FM_OF | FM_ZF | FM_SF | FM_PF;
		} else {
			*may = FM_CF | FM_OF | FM_ZF | FM_SF | FM_PF;
		}
		break;
	case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: *kills = FM_CF; break;
	case OP_BSF: case OP_BSR: *kills = FM_ZF; break;
	case OP_DAA: case OP_DAS:
		*reads = FM_CF | FM_AF;
		*kills = FM_CF | FM_AF | FM_ZF | FM_SF | FM_PF;
		break;
	case OP_AAA: case OP_AAS:
		*reads = FM_AF;
		*kills = FM_CF | FM_AF;
		break;
	case OP_SAHF: *kills = FM_CF | FM_PF | FM_AF | FM_ZF | FM_SF; break;
	case OP_LAHF: *reads = FM_CF | FM_PF | FM_AF | FM_ZF | FM_SF; break;
	/* a callee can push the flags it was called with (e.g. INT 21h; PUSHF) */
	case OP_PUSHF: case OP_INT: case OP_INTO: case OP_HLT:
	case OP_CALL: case OP_CALLF: case OP_CALL_IND: case OP_CALLF_IND:
		*reads = FM_ALL;
		break;
	case OP_CLC: case OP_STC: *kills = FM_CF; break;
	case OP_CMC: *reads = *kills = FM_CF; break;
	case OP_SALC: *reads = FM_CF; break;
	case OP_JCC: case OP_SETCC: *reads = cc_reads(in->cc); break;
	case OP_LOOPE: case OP_LOOPNE: *reads = FM_ZF; break;
	case OP_CMPS: case OP_SCAS:
		if (in->rep)
			*may = FM_ALL;
		else
			*kills = FM_ALL;
		break;
	default:
		break;
	}
}

#define EXIT_LIVE FM_ALL

/* sets in.fmask of every node to the flags it writes that are read later */
static void flag_liveness(const Func *f)
{
	static uint8_t live_in[65536 + 1];
	int i, changed;

	(void)f;
	memset(live_in, 0, (size_t)nnodes);
	do {
		changed = 0;
		for (i = nnodes - 1; i >= 0; i--) {
			Node *n = &nodes[i];
			const Insn *in = &n->in;
			uint16_t nx;
			int out = 0, reads, kills, may, li;
			if (n->bad) {
				if (live_in[i] != EXIT_LIVE) {
					live_in[i] = EXIT_LIVE;
					changed = 1;
				}
				continue;
			}
			nx = (uint16_t)(n->ip + in->len);
#define SUCC(t) do { int j_ = node_at[(uint16_t)(t)]; out |= j_ >= 0 ? live_in[j_] : EXIT_LIVE; } while (0)
			switch (in->op) {
			case OP_JMP:
				SUCC(in->a.imm);
				break;
			case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
				SUCC(in->a.imm);
				SUCC(nx);
				break;
			case OP_JMP_IND: {
				int k;
				for (k = 0; k < n->ntab; k++)
					SUCC(n->tab[k]);
				out |= EXIT_LIVE;
				break;
			}
			case OP_RET: case OP_RETF: case OP_IRET: case OP_JMPF: case OP_JMPF_IND:
				out = EXIT_LIVE;
				break;
			case OP_INT:
				if (in->a.imm == 0x20)
					out = EXIT_LIVE;
				else
					SUCC(nx);
				break;
			default:
				SUCC(nx);
				break;
			}
#undef SUCC
			flag_effects(in, &reads, &kills, &may);
			n->in.fmask = (uint8_t)((kills | may) ? ((kills | may) & out) : FM_ALL);
			li = reads | (out & ~kills);
			if (li != live_in[i]) {
				live_in[i] = (uint8_t)li;
				changed = 1;
			}
		}
	} while (changed);
}

static int pending;

static void flush(FILE *o, int extra)
{
	if (pending + extra)
		fprintf(o, "\tcpu.icount += %d;\n", pending + extra);
	pending = 0;
}

static void fname(char *buf, size_t n, uint16_t cs, uint16_t ip)
{
	snprintf(buf, n, "f_%04x_%04x", cs, ip);
}

/* goto a label in this function, or tail-call another function */
static void emit_goto(FILE *o, const Func *f, uint16_t from, uint16_t t, const char *ind)
{
	char fn[32];
	if (t <= from)
		fprintf(o, "%sCHECK(0x%04x);\n", ind, t);
	if (node_at[t] >= 0) {
		fprintf(o, "%sgoto L_%04x;\n", ind, t);
		return;
	}
	if (find_func(f->cs, t) >= 0) {
		fname(fn, sizeof(fn), f->cs, t);
		fprintf(o, "%scpu.ip = 0x%04x; %s(); return;\n", ind, t, fn);
		return;
	}
	fprintf(o, "%sBAIL(0x%04x);\n", ind, t);
}

static void emit_call(FILE *o, uint16_t cs, uint16_t ip, uint16_t ret_cs, uint16_t ret_ip)
{
	char fn[32];
	if (find_func(cs, ip) >= 0) {
		fname(fn, sizeof(fn), cs, ip);
		fprintf(o, "\tsCS = 0x%04x; cpu.ip = 0x%04x; %s(); RETURNED(0x%04x, 0x%04x);\n", cs, ip, fn, ret_cs,
		        ret_ip);
	} else {
		fprintf(o, "\tcall_any(0x%04x, 0x%04x, 0x%04x, 0x%04x); RETURNED(0x%04x, 0x%04x);\n", cs, ip, ret_cs,
		        ret_ip, ret_cs, ret_ip);
	}
}

/* ---- direct C for the common instructions --------------------------------------------
 *
 * Plain expressions on the CPU state, calling the scalar primitives of ops.h
 * (alu_add, alu_shift...) with constant sizes and flag masks, so the result
 * does not depend on the C compiler folding instruction descriptors.
 */

static const char *const R8[8] = { "rAL", "rCL", "rDL", "rBL", "rAH", "rCH", "rDH", "rBH" };
static const char *const R16[8] = { "rAX", "rCX", "rDX", "rBX", "rSP", "rBP", "rSI", "rDI" };
static const char *const R32[8] = { "cpu.r[0].d", "cpu.r[1].d", "cpu.r[2].d", "cpu.r[3].d",
                                    "cpu.r[4].d", "cpu.r[5].d", "cpu.r[6].d", "cpu.r[7].d" };
static const char *const SR[6] = { "sES", "sCS", "sSS", "sDS", "cpu.s[S_FS]", "cpu.s[S_GS]" };
static const char *const RDF[5] = { "", "rd8", "rd16", "", "rd32" };
static const char *const WRF[5] = { "", "wr8", "wr16", "", "wr32" };
/* SS-relative: the stack is never video memory (cpu.h) */
static const char *const RDS[5] = { "", "rds8", "rds16", "", "rds32" };
static const char *const WRS[5] = { "", "wrs8", "wrs16", "", "wrs32" };
static const char *const TY[5] = { "", "uint8_t", "uint16_t", "", "uint32_t" };

static const char *rname(int reg, int sz)
{
	return sz == 1 ? R8[reg] : sz == 2 ? R16[reg] : R32[reg];
}

static void off_expr(const Operand *o, char *b, size_t n)
{
	static const char *const base[8] = { "rBX + rSI", "rBX + rDI", "rBP + rSI", "rBP + rDI",
	                                     "rSI", "rDI", "rBP", "rBX" };
	if (o->ea == EA_DISP)
		snprintf(b, n, "0x%04x", o->disp);
	else if (o->disp == 0 && o->ea >= EA_SI)
		snprintf(b, n, "%s", base[o->ea]);
	else if (o->disp == 0)
		snprintf(b, n, "(uint16_t)(%s)", base[o->ea]);
	else
		snprintf(b, n, "(uint16_t)(%s + 0x%04x)", base[o->ea], o->disp);
}

static void lin_expr(const Operand *o, char *b, size_t n)
{
	char off[64];
	off_expr(o, off, sizeof(off));
	snprintf(b, n, "LIN(%s, %s)", SR[o->seg], off);
}

/* an expression reading o (memory through `addr` if given) */
static void rd_expr(const Operand *o, int sz, const char *addr, char *b, size_t n)
{
	char l[128];
	switch (o->type) {
	case OT_REG: snprintf(b, n, "%s", rname(o->reg, sz)); break;
	case OT_SREG: snprintf(b, n, "%s", SR[o->reg]); break;
	case OT_IMM: snprintf(b, n, "0x%xu", o->imm); break;
	default:
		if (addr)
			snprintf(l, sizeof(l), "%s", addr);
		else
			lin_expr(o, l, sizeof(l));
		snprintf(b, n, "%s(%s)", o->seg == S_SS ? RDS[sz] : RDF[sz], l);
		break;
	}
}

/* a statement writing v to o */
static void wr_stmt(FILE *f, const Operand *o, int sz, const char *addr, const char *v)
{
	char l[128];
	switch (o->type) {
	case OT_REG: fprintf(f, "%s = (%s)(%s);", rname(o->reg, sz), TY[sz], v); break;
	case OT_SREG: fprintf(f, "%s = (uint16_t)(%s);", SR[o->reg], v); break;
	default:
		if (addr)
			snprintf(l, sizeof(l), "%s", addr);
		else
			lin_expr(o, l, sizeof(l));
		fprintf(f, "%s(%s, (%s)(%s));", o->seg == S_SS ? WRS[sz] : WRF[sz], l, TY[sz], v);
		break;
	}
}

/* read-modify-write of operand a: opens a block with the address in A */
static const char *rmw_open(FILE *f, const Operand *a)
{
	char l[128];
	if (a->type != OT_MEM) {
		fprintf(f, "\t{ ");
		return NULL;
	}
	lin_expr(a, l, sizeof(l));
	fprintf(f, "\t{ uint32_t A = %s; ", l);
	return "A";
}

static int plain_size(int sz)
{
	return sz == 1 || sz == 2 || sz == 4;
}

/* returns 1 if emitted; pending is maintained by the caller */
static int emit_direct(FILE *o, const Insn *in, uint16_t ip)
{
	const int sz = in->osz, m = in->fmask;
	char a[200], b[200], c[512];
	const char *A;

	switch (in->op) {
	case OP_MOV: {
		int bs = in->b.type == OT_SREG ? 2 : sz;
		if (!plain_size(sz))
			return 0;
		rd_expr(&in->b, bs, NULL, b, sizeof(b));
		fprintf(o, "\t");
		wr_stmt(o, &in->a, in->a.type == OT_SREG ? 2 : sz, NULL, b);
		fprintf(o, "\n");
		return 1;
	}
	case OP_LEA:
		off_expr(&in->b, b, sizeof(b));
		fprintf(o, "\t%s = (%s)(%s);\n", rname(in->a.reg, sz), TY[sz], b);
		return 1;
	case OP_ADD: case OP_OR: case OP_ADC: case OP_SBB: case OP_AND: case OP_SUB: case OP_XOR:
	case OP_CMP: case OP_TEST: {
		const char *fn = NULL, *op = NULL, *carry = "0";
		int write = in->op != OP_CMP && in->op != OP_TEST;
		if (!plain_size(sz))
			return 0;
		switch (in->op) {
		case OP_ADD: fn = "alu_add"; break;
		case OP_ADC: fn = "alu_add"; carry = "cpu.cf"; break;
		case OP_SUB: case OP_CMP: fn = "alu_sub"; break;
		case OP_SBB: fn = "alu_sub"; carry = "cpu.cf"; break;
		case OP_AND: case OP_TEST: op = "&"; break;
		case OP_OR: op = "|"; break;
		default: op = "^"; break;
		}
		A = rmw_open(o, &in->a);
		rd_expr(&in->a, sz, A, a, sizeof(a));
		rd_expr(&in->b, sz, NULL, b, sizeof(b));
		if (fn)
			snprintf(c, sizeof(c), "%s(%s, %s, %s, %d, 0x%x)", fn, a, b, carry, sz, m);
		else
			snprintf(c, sizeof(c), "alu_logic((uint32_t)(%s) %s (uint32_t)(%s), %d, 0x%x)", a, op, b, sz, m);
		if (write) {
			fprintf(o, "uint32_t r = %s; ", c);
			wr_stmt(o, &in->a, sz, A, "r");
			fprintf(o, " }\n");
		} else {
			fprintf(o, "(void)%s; }\n", c);
		}
		return 1;
	}
	case OP_INC: case OP_DEC: case OP_NEG: case OP_NOT:
		if (!plain_size(sz))
			return 0;
		A = rmw_open(o, &in->a);
		rd_expr(&in->a, sz, A, a, sizeof(a));
		if (in->op == OP_INC)
			snprintf(c, sizeof(c), "alu_inc(%s, %d, 0x%x)", a, sz, m);
		else if (in->op == OP_DEC)
			snprintf(c, sizeof(c), "alu_dec(%s, %d, 0x%x)", a, sz, m);
		else if (in->op == OP_NEG)
			snprintf(c, sizeof(c), "alu_sub(0, %s, 0, %d, 0x%x)", a, sz, m);
		else
			snprintf(c, sizeof(c), "~(uint32_t)(%s)", a);
		fprintf(o, "uint32_t r = %s; ", c);
		wr_stmt(o, &in->a, sz, A, "r");
		fprintf(o, " }\n");
		return 1;
	case OP_MUL: case OP_IMUL1:
		if (!plain_size(sz))
			return 0;
		rd_expr(&in->a, sz, NULL, a, sizeof(a));
		fprintf(o, "\talu_mul1(%d, %s, %d, 0x%x);\n", in->op == OP_IMUL1, a, sz, m);
		return 1;
	case OP_IMUL:
		if (sz != 2 && sz != 4)
			return 0;
		rd_expr(&in->b, sz, NULL, b, sizeof(b));
		rd_expr(&in->c, sz, NULL, c, sizeof(c));
		fprintf(o, "\t{ uint32_t r = alu_imul(%s, %s, %d, 0x%x); %s = (%s)r; }\n", b, c, sz, m,
		        rname(in->a.reg, sz), TY[sz]);
		return 1;
	case OP_DIV: case OP_IDIV:
		if (!plain_size(sz))
			return 0;
		rd_expr(&in->a, sz, NULL, a, sizeof(a));
		fprintf(o, "\tif (unlikely(alu_div(%d, %s, %d))) { cpu.icount += %d; BAIL(0x%04x); }\n",
		        in->op == OP_IDIV, a, sz, pending, ip);
		return 1;
	case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: case OP_SHL: case OP_SHR: case OP_SAL: case OP_SAR:
		if (!plain_size(sz))
			return 0;
		if (in->b.type == OT_IMM) {
			unsigned cnt = in->b.imm & 0x1F;
			if (!cnt) {
				fprintf(o, "\t/* count 0: no effect */\n");
				return 1;
			}
			A = rmw_open(o, &in->a);
			rd_expr(&in->a, sz, A, a, sizeof(a));
			fprintf(o, "uint32_t r = alu_shift(%d, %s, %u, %d, 0x%x); ", in->op, a, cnt, sz, m);
			wr_stmt(o, &in->a, sz, A, "r");
			fprintf(o, " }\n");
		} else {
			rd_expr(&in->b, 1, NULL, b, sizeof(b));
			fprintf(o, "\t{ uint32_t n = (uint32_t)(%s) & 0x1F; if (n) ", b);
			A = rmw_open(o, &in->a);
			rd_expr(&in->a, sz, A, a, sizeof(a));
			fprintf(o, "uint32_t r = alu_shift(%d, %s, n, %d, 0x%x); ", in->op, a, sz, m);
			wr_stmt(o, &in->a, sz, A, "r");
			fprintf(o, " } }\n");
		}
		return 1;
	case OP_XCHG:
		if (!plain_size(sz) || in->b.type != OT_REG)
			return 0;
		A = rmw_open(o, &in->a);
		rd_expr(&in->a, sz, A, a, sizeof(a));
		fprintf(o, "uint32_t t = %s; ", a);
		wr_stmt(o, &in->a, sz, A, rname(in->b.reg, sz));
		fprintf(o, " %s = (%s)t; }\n", rname(in->b.reg, sz), TY[sz]);
		return 1;
	case OP_LDS: case OP_LES: case OP_LSS:
		if (sz != 2)
			return 0;
		lin_expr(&in->b, a, sizeof(a));
		fprintf(o, "\t{ uint32_t A = %s; uint16_t o_ = rd16(A), s_ = rd16(A + 2); %s = o_; %s = s_; }\n", a,
		        R16[in->a.reg], in->op == OP_LDS ? "sDS" : in->op == OP_LES ? "sES" : "sSS");
		return 1;
	case OP_PUSH:
		if (sz != 2)
			return 0;
		rd_expr(&in->a, 2, NULL, a, sizeof(a));
		fprintf(o, "\tpush16((uint16_t)(%s));\n", a);
		return 1;
	case OP_POP:
		if (sz != 2)
			return 0;
		if (in->a.type == OT_MEM) {
			lin_expr(&in->a, a, sizeof(a));
			fprintf(o, "\t{ uint16_t v_ = pop16(); wr16(%s, v_); }\n", a);
		} else {
			fprintf(o, "\t%s = pop16();\n", in->a.type == OT_SREG ? SR[in->a.reg] : R16[in->a.reg]);
		}
		return 1;
	case OP_PUSHF:
		if (sz != 2)
			return 0;
		fprintf(o, "\tpush16(flags_get());\n");
		return 1;
	case OP_POPF:
		if (sz != 2)
			return 0;
		fprintf(o, "\tflags_set(pop16());\n");
		return 1;
	case OP_ENTER:
		if ((in->b.imm & 31) != 0 || sz != 2)
			return 0;
		fprintf(o, "\tpush16(rBP); rBP = rSP; rSP = (uint16_t)(rSP - 0x%x);\n", in->a.imm & 0xFFFF);
		return 1;
	case OP_CBW:
		if (sz != 2)
			return 0;
		fprintf(o, "\trAX = (uint16_t)(int16_t)(int8_t)rAL;\n");
		return 1;
	case OP_CWD:
		if (sz != 2)
			return 0;
		fprintf(o, "\trDX = (rAX & 0x8000) ? 0xFFFF : 0;\n");
		return 1;
	case OP_XLAT:
		fprintf(o, "\trAL = rd8(LIN(%s, (uint16_t)(rBX + rAL)));\n", SR[in->seg]);
		return 1;
	case OP_IN: case OP_OUT: {
		const char *port = in->b.type == OT_IMM ? NULL : "rDX";
		char pb[16];
		if (!plain_size(sz))
			return 0;
		if (!port) {
			snprintf(pb, sizeof(pb), "0x%02x", in->b.imm);
			port = pb;
		}
		if (in->op == OP_IN)
			fprintf(o, "\t%s = (%s)port_in(%s, %d);\n", rname(R_AX, sz), TY[sz], port, sz);
		else
			fprintf(o, "\tport_out(%s, %s, %d);\n", port, rname(R_AX, sz), sz);
		return 1;
	}
	case OP_CLC: fprintf(o, "\tcpu.cf = 0;\n"); return 1;
	case OP_STC: fprintf(o, "\tcpu.cf = 1;\n"); return 1;
	case OP_CMC: fprintf(o, "\tcpu.cf ^= 1;\n"); return 1;
	case OP_CLD: fprintf(o, "\tcpu.df = 0;\n"); return 1;
	case OP_STD: fprintf(o, "\tcpu.df = 1;\n"); return 1;
	case OP_CLI: fprintf(o, "\tcpu.ifl = 0;\n"); return 1;
	case OP_STI: fprintf(o, "\tcpu.ifl = 1;\n"); return 1;
	case OP_MOVS: fprintf(o, "\tstr_movs(%d, %d, 0x%x);\n", sz, in->seg, in->rep); return 1;
	case OP_STOS: fprintf(o, "\tstr_stos(%d, 0x%x);\n", sz, in->rep); return 1;
	case OP_LODS: fprintf(o, "\tstr_lods(%d, %d, 0x%x);\n", sz, in->seg, in->rep); return 1;
	case OP_CMPS: fprintf(o, "\tstr_cmps(%d, %d, 0x%x);\n", sz, in->seg, in->rep); return 1;
	case OP_SCAS: fprintf(o, "\tstr_scas(%d, 0x%x);\n", sz, in->rep); return 1;
	case OP_INS: fprintf(o, "\tstr_ins(%d, 0x%x);\n", sz, in->rep); return 1;
	case OP_OUTS: fprintf(o, "\tstr_outs(%d, %d, 0x%x);\n", sz, in->seg, in->rep); return 1;
	default:
		return 0;
	}
}

static void emit_node(FILE *o, const Func *f, const Node *n)
{
	const Insn *in = &n->in;
	uint16_t ip = n->ip, nx = (uint16_t)(ip + in->len), cs = f->cs;
	const char *ex;
	char txt[96];

	if (n->bad) {
		fprintf(o, "\t/* %04x: undecodable */\n", ip);
		flush(o, 0);
		fprintf(o, "\tBAIL(0x%04x);\n", ip);
		return;
	}
	x86_format(in, cs, ip, txt, sizeof(txt));
	for (char *p = txt; *p; p++)
		if (*p == '*' || *p == '/')
			*p = '.';
	fprintf(o, "\t/* %04x: %s */\n", ip, txt);

	switch (in->op) {
	case OP_NOP: case OP_WAIT:
		pending++;
		return;
	case OP_LAHF: fprintf(o, "\texec_lahf();\n"); pending++; return;
	case OP_SAHF: fprintf(o, "\texec_sahf();\n"); pending++; return;
	case OP_LEAVE: fprintf(o, "\texec_leave();\n"); pending++; return;
	case OP_SALC: fprintf(o, "\texec_salc();\n"); pending++; return;
	case OP_DAA: fprintf(o, "\tbcd_daa();\n"); pending++; return;
	case OP_DAS: fprintf(o, "\tbcd_das();\n"); pending++; return;
	case OP_AAA: fprintf(o, "\tbcd_aaa();\n"); pending++; return;
	case OP_AAS: fprintf(o, "\tbcd_aas();\n"); pending++; return;
	case OP_FPU_SHORTCUT: fprintf(o, "\tfpu_shortcut(0x%x);\n", in->a.imm); pending++; return;
	case OP_FPU:
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " if (unlikely(fpu_exec(&I))) { cpu.icount += %d; BAIL(0x%04x); } }\n", pending, ip);
		pending++;
		return;
	case OP_DIV: case OP_IDIV:
		if (emit_direct(o, in, ip)) {
			pending++;
			return;
		}
		/* fall through */
	case OP_AAM: case OP_BOUND:
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " if (unlikely(%s(&I))) { cpu.icount += %d; BAIL(0x%04x); } }\n",
		        in->op == OP_AAM ? "exec_aam" : in->op == OP_BOUND ? "exec_bound" : "exec_div", pending, ip);
		pending++;
		return;
	case OP_IN: case OP_OUT: case OP_INS: case OP_OUTS:
		/* time-observing: the count must be exact here */
		flush(o, 0);
		if (emit_direct(o, in, ip)) {
			pending = 1;
			return;
		}
		ex = exec_fn(in->op);
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " %s(&I); }\n", ex);
		pending = 1;
		return;
	case OP_JMP:
		flush(o, 1);
		emit_goto(o, f, ip, (uint16_t)in->a.imm, "\t");
		return;
	case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: {
		flush(o, 1);
		switch (in->op) {
		case OP_JCC: fprintf(o, "\tif (cond(%d)) {\n", in->cc); break;
		case OP_JCXZ: fprintf(o, "\tif (rCX == 0) {\n"); break;
		case OP_LOOP: fprintf(o, "\tif (--rCX != 0) {\n"); break;
		case OP_LOOPE: fprintf(o, "\tif ((--rCX != 0) & cpu.zf) {\n"); break;
		default: fprintf(o, "\tif ((--rCX != 0) & !cpu.zf) {\n"); break;
		}
		emit_goto(o, f, ip, (uint16_t)in->a.imm, "\t\t");
		fprintf(o, "\t}\n");
		return;
	}
	case OP_CALL:
		flush(o, 1);
		fprintf(o, "\tpush16(0x%04x);\n", nx);
		emit_call(o, cs, (uint16_t)in->a.imm, cs, nx);
		return;
	case OP_CALLF:
		flush(o, 1);
		fprintf(o, "\tpush16(0x%04x); push16(0x%04x);\n", cs, nx);
		emit_call(o, in->a.disp, (uint16_t)in->a.imm, cs, nx);
		return;
	case OP_CALL_IND:
		flush(o, 1);
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " uint16_t t = (uint16_t)opnd_read(&I.a, 2); push16(0x%04x); "
		           "call_any(0x%04x, t, 0x%04x, 0x%04x); RETURNED(0x%04x, 0x%04x); }\n",
		        nx, cs, cs, nx, cs, nx);
		return;
	case OP_CALLF_IND:
		flush(o, 1);
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " uint32_t a = ea_linear(&I.a); uint16_t to = rd16(a), ts = rd16(a + 2); "
		           "push16(0x%04x); push16(0x%04x); call_any(ts, to, 0x%04x, 0x%04x); RETURNED(0x%04x, 0x%04x); }\n",
		        cs, nx, cs, nx, cs, nx);
		return;
	case OP_RET:
		flush(o, 1);
		if (in->a.imm)
			fprintf(o, "\tcpu.ip = pop16(); rSP += 0x%x; return;\n", in->a.imm);
		else
			fprintf(o, "\tcpu.ip = pop16(); return;\n");
		return;
	case OP_RETF:
		flush(o, 1);
		fprintf(o, "\tcpu.ip = pop16(); sCS = pop16();");
		if (in->a.imm)
			fprintf(o, " rSP += 0x%x;", in->a.imm);
		fprintf(o, " return;\n");
		return;
	case OP_IRET:
		flush(o, 1);
		fprintf(o, "\tcpu.ip = pop16(); sCS = pop16(); flags_set(pop16()); return;\n");
		return;
	case OP_JMPF:
		flush(o, 1);
		fprintf(o, "\tJUMP_ANY(0x%04x, 0x%04x);\n", in->a.disp, (uint16_t)in->a.imm);
		return;
	case OP_JMP_IND:
		flush(o, 1);
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " uint16_t t = (uint16_t)opnd_read(&I.a, 2);\n");
		if (n->ntab > 0) {
			int k, j;
			fprintf(o, "\tswitch (t) {\n");
			for (k = 0; k < n->ntab; k++) {
				int dup = 0;
				for (j = 0; j < k; j++)
					if (n->tab[j] == n->tab[k])
						dup = 1;
				if (dup)
					continue;
				fprintf(o, "\tcase 0x%04x:\n", n->tab[k]);
				if (node_at[n->tab[k]] >= 0)
					fprintf(o, "\t\tgoto L_%04x;\n", n->tab[k]);
				else
					fprintf(o, "\t\tJUMP_ANY(0x%04x, 0x%04x);\n", cs, n->tab[k]);
			}
			fprintf(o, "\tdefault: break;\n\t}\n");
		}
		fprintf(o, "\tJUMP_ANY(0x%04x, t); }\n", cs);
		return;
	case OP_JMPF_IND:
		flush(o, 1);
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " uint32_t a = ea_linear(&I.a); JUMP_ANY(rd16(a + 2), rd16(a)); }\n");
		return;
	case OP_INT:
		flush(o, 1);
		fprintf(o, "\tcompiled_int(0x%02x, 0x%04x);\n", in->a.imm, nx);
		return;
	case OP_INTO:
		flush(o, 1);
		fprintf(o, "\tif (cpu.of) compiled_int(4, 0x%04x);\n", nx);
		return;
	case OP_HLT:
		flush(o, 1);
		fprintf(o, "\tcompiled_hlt(0x%04x);\n", nx);
		return;
	default:
		if (emit_direct(o, in, ip)) {
			pending++;
			return;
		}
		ex = exec_fn(in->op);
		if (!ex) {
			flush(o, 0);
			fprintf(o, "\tBAIL(0x%04x); /* not handled by the recompiler */\n", ip);
			return;
		}
		fprintf(o, "\t{ ");
		insn_const(o, in);
		fprintf(o, " %s(&I); }\n", ex);
		pending++;
		return;
	}
}

static int falls_through(const Node *n)
{
	if (n->bad)
		return 0;
	switch (n->in.op) {
	case OP_JMP: case OP_JMPF: case OP_JMP_IND: case OP_JMPF_IND: case OP_RET: case OP_RETF: case OP_IRET:
		return 0;
	case OP_INT:
		return n->in.a.imm != 0x20;
	default:
		return 1;
	}
}

static void emit_function(FILE *o, Func *f)
{
	int i, nres = 0;
	char fn[32];

	explore(f, 1);
	qsort(nodes, (size_t)nnodes, sizeof(Node), cmp_node);
	memset(node_at, 0xFF, sizeof(node_at));
	for (i = 0; i < nnodes; i++)
		node_at[nodes[i].ip] = i;
	/* labels: branch targets, the entry, and fallthroughs into non-adjacent code */
	nodes[node_at[f->ip]].label = 1;
	for (i = 0; i < nnodes; i++) {
		const Node *n = &nodes[i];
		uint16_t nx = (uint16_t)(n->ip + n->in.len);
		if (n->bad)
			continue;
		switch (n->in.op) {
		case OP_JMP: case OP_JCC: case OP_JCXZ: case OP_LOOP: case OP_LOOPE: case OP_LOOPNE:
			if (node_at[(uint16_t)n->in.a.imm] >= 0)
				nodes[node_at[(uint16_t)n->in.a.imm]].label = 1;
			break;
		case OP_JMP_IND: {
			int k;
			for (k = 0; k < n->ntab; k++)
				if (node_at[n->tab[k]] >= 0)
					nodes[node_at[n->tab[k]]].label = 1;
			break;
		}
		default:
			break;
		}
		if (falls_through(n) && node_at[nx] >= 0 && (i + 1 >= nnodes || nodes[i + 1].ip != nx))
			nodes[node_at[nx]].label = 1;
	}

	/* resume points: the interpreter may hand back to us there */
	for (i = 0; i < nents; i++) {
		if (ents[i].is_resume && ents[i].owner_cs == f->cs && ents[i].owner_ip == f->ip &&
		    node_at[ents[i].ip] >= 0) {
			nodes[node_at[ents[i].ip]].label = 1;
			nres++;
		}
	}

	flag_liveness(f);

	fname(fn, sizeof(fn), f->cs, f->ip);
	fprintf(o, "\nvoid %s(void)\n{\n", fn);
	if (nres) {
		fprintf(o, "\tswitch (cpu.ip) {\n");
		for (i = 0; i < nents; i++)
			if (ents[i].is_resume && ents[i].owner_cs == f->cs && ents[i].owner_ip == f->ip &&
			    node_at[ents[i].ip] >= 0)
				fprintf(o, "\tcase 0x%04x: goto L_%04x;\n", ents[i].ip, ents[i].ip);
		fprintf(o, "\tdefault: break;\n\t}\n");
	}
	fprintf(o, "\tgoto L_%04x;\n", f->ip);
	pending = 0;
	for (i = 0; i < nnodes; i++) {
		const Node *n = &nodes[i];
		uint16_t nx = (uint16_t)(n->ip + n->in.len);
		if (n->label)
			fprintf(o, "L_%04x:;\n", n->ip);
		emit_node(o, f, n);
		if (!falls_through(n)) {
			pending = 0;
			continue;
		}
		if (i + 1 < nnodes && nodes[i + 1].ip == nx && !nodes[i + 1].label)
			continue;
		flush(o, 0);
		if (i + 1 < nnodes && nodes[i + 1].ip == nx)
			continue; /* falls into the label below */
		if (node_at[nx] >= 0)
			fprintf(o, "\tgoto L_%04x;\n", nx);
		else
			fprintf(o, "\tBAIL(0x%04x);\n", nx);
	}
	fprintf(o, "}\n");
}

static int table_size;

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static int cmp_func(const void *a, const void *b)
{
	const Func *x = a, *y = b;
	if (x->cs != y->cs)
		return (int)x->cs - (int)y->cs;
	return (int)x->ip - (int)y->ip;
}

int main(int argc, char **argv)
{
	int i, added, code_ptrs = 0;
	char path[1024];
	FILE *hdr, *idx, *o = NULL;
	uint16_t cur_seg = 0;
	uint32_t sum;

	if (argc < 3) {
		fprintf(stderr, "usage: recomp SCORCH.EXE OUTDIR [entries.txt ...]\n");
		return 2;
	}
	memset(fhash, 0xFF, sizeof(fhash));
	if (load(argv[1]) != 0)
		return 1;
	sum = image_checksum();
	add_func(entry_cs, entry_ip, 0);
	for (i = 3; i < argc; i++)
		load_entries(argv[i]);
	if (nents)
		qsort(ents, (size_t)nents, sizeof(Entry), cmp_ent);

	for (;;) {
		int fresh = 0, resumes = 0, k;

		/* discovery: explore until no new functions, then look for data pointers */
		for (;;) {
			for (i = 0; i < nfuncs; i++) {
				if (!funcs[i].explored) {
					funcs[i].explored = 1;
					explore(&funcs[i], 0);
					code_ptrs += scan_code_pointers(funcs[i].cs);
				}
			}
			added = scan_far_pointers();
			if (!added)
				break;
		}
		/* runtime addresses: inside a function we have, they are resume points
		 * (no copy of the code); otherwise they start new functions */
		own_clear();
		for (i = 0; i < nfuncs; i++) {
			explore(&funcs[i], 1);
			for (k = 0; k < nnodes; k++) {
				int32_t *slot = own_slot(okey(funcs[i].cs, nodes[k].ip));
				if (!nodes[k].bad && *slot < 0)
					*slot = i;
			}
		}
		for (i = 0; i < nents; i++) {
			int32_t o2;
			ents[i].is_resume = 0;
			if (find_func(ents[i].cs, ents[i].ip) >= 0)
				continue;
			o2 = *own_slot(okey(ents[i].cs, ents[i].ip));
			if (o2 >= 0 && funcs[o2].cs == ents[i].cs) {
				ents[i].is_resume = 1;
				ents[i].owner_cs = funcs[o2].cs;
				ents[i].owner_ip = funcs[o2].ip;
				resumes++;
			} else {
				int nf = add_func(ents[i].cs, ents[i].ip, 2);
				fresh++;
				/* entries are in address order: the later ones its code reaches
				 * become its resume points, not functions of their own (copies) */
				if (nf >= 0) {
					explore(&funcs[nf], 1);
					for (k = 0; k < nnodes; k++) {
						int32_t *slot = own_slot(okey(funcs[nf].cs, nodes[k].ip));
						if (!nodes[k].bad && *slot < 0)
							*slot = nf;
					}
				}
			}
		}
		fprintf(stderr, "%d functions (%d from far pointers in code), %d resume points, %d new\n", nfuncs,
		        code_ptrs, resumes, fresh);
		if (!fresh)
			break;
	}

	qsort(funcs, (size_t)nfuncs, sizeof(Func), cmp_func);
	memset(fhash, 0xFF, sizeof(fhash));
	{
		int n = nfuncs;
		nfuncs = 0;
		for (i = 0; i < n; i++) {
			Func f = funcs[i];
			int k = add_func(f.cs, f.ip, f.from_table);
			funcs[k].explored = 1;
		}
	}

	snprintf(path, sizeof(path), "%s/funcs.h", argv[2]);
	hdr = fopen(path, "w");
	if (!hdr) {
		perror(path);
		return 1;
	}
	fprintf(hdr, "/* generated by tools/recomp.c from %s: do not edit, do not distribute */\n", argv[1]);
	for (i = 0; i < nfuncs; i++)
		fprintf(hdr, "void f_%04x_%04x(void);\n", funcs[i].cs, funcs[i].ip);
	fclose(hdr);

	for (i = 0; i < nfuncs; i++) {
		Func *f = &funcs[i];
		if (!o || f->cs != cur_seg) {
			if (o)
				fclose(o);
			cur_seg = f->cs;
			snprintf(path, sizeof(path), "%s/seg_%04x.c", argv[2], cur_seg);
			o = fopen(path, "w");
			if (!o) {
				perror(path);
				return 1;
			}
			fprintf(o, "/* generated by tools/recomp.c: code segment %04x of the game */\n", cur_seg);
			fprintf(o, "#include \"gen.h\"\n#include \"funcs.h\"\n");
		}
		emit_function(o, f);
	}
	if (o)
		fclose(o);

	snprintf(path, sizeof(path), "%s/index.c", argv[2]);
	idx = fopen(path, "w");
	if (!idx) {
		perror(path);
		return 1;
	}
	fprintf(idx, "/* generated by tools/recomp.c */\n#include \"gen.h\"\n#include \"funcs.h\"\n\n");
	fprintf(idx, "const char *compiled_info = \"%d functions recompiled (image %08x)\";\n", nfuncs, sum);
	fprintf(idx, "const uint32_t compiled_image_sum = 0x%08xu;\n\n", sum);
	{
		/* functions and resume points, sorted by (cs, ip) */
		int n = 0, k;
		uint64_t *keys = malloc(sizeof(uint64_t) * (size_t)(nfuncs + nents));
		for (i = 0; i < nfuncs; i++)
			keys[n++] = ((uint64_t)okey(funcs[i].cs, funcs[i].ip) << 32) | okey(funcs[i].cs, funcs[i].ip);
		for (i = 0; i < nents; i++)
			if (ents[i].is_resume)
				keys[n++] = ((uint64_t)okey(ents[i].cs, ents[i].ip) << 32) |
				            okey(ents[i].owner_cs, ents[i].owner_ip);
		qsort(keys, (size_t)n, sizeof(uint64_t), cmp_u64);
		/* drop duplicates (an address listed twice) */
		for (i = 0, k = 0; i < n; i++)
			if (k == 0 || (keys[k - 1] >> 32) != (keys[i] >> 32))
				keys[k++] = keys[i];
		n = k;
		table_size = n;
		fprintf(idx, "static const struct { uint32_t key; CompiledFn fn; } table[%d] = {\n", n);
		for (i = 0; i < n; i++) {
			uint32_t fk = (uint32_t)keys[i];
			fprintf(idx, "\t{0x%08xu, f_%04x_%04x},\n", (uint32_t)(keys[i] >> 32), fk >> 16, fk & 0xFFFF);
		}
		fprintf(idx, "};\n\n");
		free(keys);
	}
	fprintf(idx,
	        "static int enabled = 1;\n\n"
	        "void compiled_disable(void)\n{\n\tenabled = 0;\n}\n\n"
	        "void compiled_init(void)\n{\n}\n\n"
	        "CompiledFn compiled_lookup(uint16_t cs, uint16_t ip)\n{\n"
	        "\tuint32_t key = ((uint32_t)cs << 16) | ip;\n"
	        "\tint lo = 0, hi = %d - 1;\n"
	        "\tif (!enabled)\n\t\treturn NULL;\n"
	        "\twhile (lo <= hi) {\n"
	        "\t\tint mid = (lo + hi) >> 1;\n"
	        "\t\tif (table[mid].key == key)\n\t\t\treturn table[mid].fn;\n"
	        "\t\tif (table[mid].key < key)\n\t\t\tlo = mid + 1;\n\t\telse\n\t\t\thi = mid - 1;\n"
	        "\t}\n\treturn NULL;\n}\n",
	        table_size);
	fclose(idx);
	fprintf(stderr, "wrote %d functions to %s (image checksum %08x)\n", nfuncs, argv[2], sum);
	return 0;
}
