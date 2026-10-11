/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * BandBench: each kind of banded work timed with 0, 1, 2, 4 and 8 cores.
 *
 * The kinds are the loops that use omc_bands() (include/openmulticore/bands.h)
 * today, each a copy of the band function its program runs:
 *   1 cover        OpenBench's cover(): a 1600 x 1200 24-bit picture cut and
 *                  scaled to cover 1920 x 1080 (an OpenUp backdrop);
 *   2 over         OpenBench's over_gradient(): a 1920 x 1080 picture with
 *                  alpha laid over the theme's gradient, in place;
 *   3 cover+over   OpenBench 0.1.16's backdrop_rows(), both in one pass, on a
 *                  1600 x 1200 picture with alpha;
 *   4 png rgb      openpng.datatype 47.4's png_rows(): 1920 x 1080 RGB rows
 *                  (8 bits a sample, inflated and unfiltered) to ARGB;
 *   5 png rgba     the same, RGBA;
 *   6 adam7        the same, RGBA interlaced: Adam7's seven passes placed;
 *   7 small        kind 3 at 64 x 64, what a call costs with nothing to share.
 * Each kind runs:
 *   0      every row on the caller (OMCBF_LOCAL): what a machine without
 *          openmulticore.library does;
 *   cpu0   the bands through the library aimed at the main CPU
 *          (OMCBF_LIBCPU0): what the library does with no cores board;
 *   1 2 4 8   at most that many bands to the cores board's cores
 *          (OMCBF_MAXCORES), the caller running a band of its own beside
 *          them; first taking back the bands not finished when the caller
 *          comes to them (omc_bands' default, what the programs do), then,
 *          unless QUICK, waiting for every band (OMCBF_WAIT), which shows
 *          the cores' own speed.
 * Every way must give the bytes of way 0, or the kind says DIFFERENT and the
 * run fails. Times are milliseconds from the E clock, the best of REPS runs
 * (the picture made afresh outside the timing before each one). A column
 * past the board's cores shows "-".
 *
 *   BandBench [REPS n] [KIND n] [QUICK]
 *
 * Built by library/build.sh. 11 October 2026, the Team. */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <openmulticore/bands.h>

struct Device *TimerBase;
static struct timerequest treq;
static ULONG efreq;

static unsigned long long ticks(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return ((unsigned long long)e.ev_hi << 32) | e.ev_lo;
}

static ULONG us_since(unsigned long long t0)
{
    return (ULONG)((ticks() - t0) * 1000000ULL / efreq);
}

/* ---- the band functions: jobs by the rules (no OS calls, no globals written, only their rows) ---- */

struct backdrop {
    const ULONG *src;               /* the picture, sw x sh */
    LONG sw, sh, vw, vh, ox, oy;    /* the part of it that covers the screen */
    const ULONG *grad;              /* the gradient, W x H, or NULL */
    ULONG *dst;                     /* W x H: the picture itself when it is that size */
    LONG W, H;
    ULONG opaque;                   /* 0xff000000 for a picture without alpha, else 0 */
};

/* OpenBench 0.1.16's backdrop_rows() (openamigabench desktop/openbench.c), the same code. With
 * opaque set and a picture of another size it is 0.1.14's cover(); with a picture the screen's
 * size and alpha it is over_gradient(); with both, the backdrop as OpenBench makes it. */
static void backdrop_rows(APTR data, ULONG y0, ULONG y1)
{
    const struct backdrop *b = data;
    ULONG y;
    LONG x;
    for (y = y0; y < y1; y++) {
        ULONG *row = b->dst + y * b->W;
        const ULONG *g = b->grad ? b->grad + y * b->W : NULL;
        if (b->src != b->dst) {
            const ULONG *s = b->src + (b->oy + (LONG)y * b->vh / b->H) * b->sw + b->ox;
            for (x = 0; x < b->W; x++) row[x] = s[x * b->vw / b->W] | b->opaque;
        } else if (b->opaque) {
            for (x = 0; x < b->W; x++) row[x] |= b->opaque;
        }
        if (b->opaque) continue;
        for (x = 0; x < b->W; x++) {
            ULONG p = row[x], a = p >> 24, q, r, gg, bb;
            if (a == 255) continue;
            q = g ? g[x] : 0;
            r = (((p >> 16) & 255) * a + ((q >> 16) & 255) * (255 - a) + 127) / 255;
            gg = (((p >> 8) & 255) * a + ((q >> 8) & 255) * (255 - a) + 127) / 255;
            bb = ((p & 255) * a + (q & 255) * (255 - a) + 127) / 255;
            row[x] = 0xff000000UL | (r << 16) | (gg << 8) | bb;
        }
    }
}

/* openpng.datatype 47.4's colour stage (openamigaimage Datatypes/png/pngdecode.c, png_rows() and
 * convert()), cut down to the two colour types the desktop's pictures use: 2 (RGB) and 6 (RGBA), 8
 * bits a sample, no tRNS key. raw is what png_unpack() leaves: each row a filter byte and its samples,
 * the seven passes one after the other when interlaced. */
static const UBYTE adam7[7][4] = {      /* x start, y start, x step, y step (pngdecode.c's table) */
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 }
};

struct pngwork {
    const UBYTE *raw;
    ULONG got;                      /* bytes of raw that came */
    ULONG *argb;                    /* w x h */
    ULONG w, h, bpp;                /* bpp: 3 (RGB) or 4 (RGBA) */
    ULONG interlace;
};

static void pass_size(const struct pngwork *p, LONG pass, ULONG *pw, ULONG *ph)
{
    if (!p->interlace) {
        *pw = p->w;
        *ph = p->h;
        return;
    }
    *pw = p->w > adam7[pass][0] ? (p->w - adam7[pass][0] + adam7[pass][2] - 1) / adam7[pass][2] : 0;
    *ph = p->h > adam7[pass][1] ? (p->h - adam7[pass][1] + adam7[pass][3] - 1) / adam7[pass][3] : 0;
    if (!*pw || !*ph)
        *pw = *ph = 0;
}

static void convert(const struct pngwork *p, const UBYTE *s, ULONG n, ULONG *d, ULONG step)
{
    ULONG x;
    if (p->bpp == 3)
        for (x = 0; x < n; x++, s += 3, d += step)
            *d = 0xff000000UL | ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    else
        for (x = 0; x < n; x++, s += 4, d += step)
            *d = ((ULONG)s[3] << 24) | ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
}

static void png_rows(APTR data, ULONG y0, ULONG y1)
{
    const struct pngwork *p = data;
    ULONG w = p->w, y;
    if (y1 > p->h)
        y1 = p->h;
    if (!p->interlace) {
        ULONG rb = w * p->bpp;
        for (y = y0; y < y1; y++) {
            ULONG at = y * (rb + 1), *d = p->argb + y * w, x;
            if (at < p->got)
                convert(p, p->raw + at + 1, w, d, 1);
            else
                for (x = 0; x < w; x++)
                    d[x] = 0;
        }
    } else {
        ULONG at = 0, i;
        LONG pass;
        for (i = y0 * w; i < y1 * w; i++)
            p->argb[i] = 0;
        for (pass = 0; pass < 7; pass++) {
            ULONG pw, ph, rb;
            pass_size(p, pass, &pw, &ph);
            if (!pw)
                continue;
            rb = pw * p->bpp;
            for (y = 0; y < ph; y++, at += rb + 1) {
                ULONG ty = adam7[pass][1] + y * adam7[pass][3];
                if (at >= p->got)
                    return;
                if (ty >= y0 && ty < y1)
                    convert(p, p->raw + at + 1, pw, p->argb + ty * w + adam7[pass][0], adam7[pass][2]);
            }
        }
    }
}

/* ---- the bench --------------------------------------------------------------------------------- */

#define NWAYS 6                     /* 0, cpu0, 1, 2, 4, 8 */
static const ULONG way_cores[NWAYS] = { 0, 0, 1, 2, 4, 8 };

struct kind {
    const char *name;
    omc_band_fn fn;
    APTR data;
    ULONG rows, stride;
    APTR out;                       /* what the bands write, on a 16-byte line */
    ULONG words;                    /* its size in longwords, for the sum */
    const ULONG *fresh;             /* in place: the picture copied over out before each run, else NULL */
};

static ULONG cores;                 /* the board's, from OMC_CoreCount */

static ULONG sum(const ULONG *p, ULONG n)
{
    ULONG h = 2166136261UL;
    while (n--) h = (h ^ *p++) * 16777619UL;
    return h;
}

static void show_ms(ULONG us)
{
    if (us == ~0UL) PutStr((STRPTR)"       -");
    else Printf((STRPTR)" %5ld.%ld", (LONG)(us / 1000), (LONG)((us % 1000) / 100));
}

/* One way REPS times: the best time, the stats of the last run, the sum of what it made. */
static ULONG run_way(struct Library *omc, const struct kind *k, ULONG flags, ULONG reps, struct OMCBandStats *st, ULONG *got)
{
    ULONG best = ~0UL, r;
    for (r = 0; r < reps; r++) {
        unsigned long long t0;
        ULONG t;
        if (k->fresh) CopyMemQuick((APTR)k->fresh, k->out, k->words * 4);
        else SetMem(k->out, 0x55, k->words * 4);     /* nothing of the last run survives a band left unwritten */
        omc__rest = 0;                               /* every run tries the board: what one call costs */
        t0 = ticks();
        omc_bands(omc, k->fn, k->data, k->rows, k->out, k->stride, flags, st);
        t = us_since(t0);
        if (t < best) best = t;
    }
    *got = sum(k->out, k->words);
    return best;
}

/* A kind with every way. 0 when every way gave way 0's bytes. */
static int bench(struct Library *omc, const struct kind *k, ULONG reps, int quick)
{
    ULONG want, got, t[NWAYS], mode, w;
    struct OMCBandStats st = { 0, 0, 0, 0, 0 }, most = { 0, 0, 0, 0, 0 };
    int bad = 0;
    if (!omc) quick = 1;                             /* nothing to wait for */
    Printf((STRPTR)"%-11s", (LONG)k->name);
    t[0] = run_way(omc, k, OMCBF_LOCAL, reps, &st, &want);
    for (mode = 0; mode < (quick ? 1UL : 2UL); mode++) {
        ULONG wf = mode ? OMCBF_WAIT : 0, last = 0;
        for (w = 1; w < NWAYS; w++) {
            t[w] = ~0UL;
            if (!omc) continue;
            if (w == 1) {
                if (mode) continue;                  /* cpu0 never takes a band back: once is enough */
                t[w] = run_way(omc, k, OMCBF_LIBCPU0, reps, &st, &got);
            } else {
                if (way_cores[w] > cores) continue;
                t[w] = run_way(omc, k, wf | OMCBF_MAXCORES(way_cores[w]), reps, &st, &got);
                most = st;
                last = way_cores[w];
            }
            if (got != want) bad = 1;
        }
        if (mode) PutStr((STRPTR)"\n  waiting  ");
        show_ms(mode ? ~0UL : t[0]);
        for (w = 1; w < NWAYS; w++) show_ms(t[w]);
        if (last)
            Printf((STRPTR)"   at %ld: %ld bands, %ld on the board, %ld taken back", (LONG)last,
                   (LONG)most.obs_Bands, (LONG)most.obs_Board, (LONG)most.obs_Taken);
    }
    Printf((STRPTR)"   %s\n", (LONG)(bad ? "DIFFERENT" : "same"));
    return bad;
}

/* A buffer on a 16-byte line of its own (bands.h), Fast RAM when there is any; its block for FreeVec. */
static APTR alloc_line(ULONG bytes, APTR *block)
{
    UBYTE *m = AllocVec(bytes + 15, MEMF_FAST | MEMF_CLEAR);
    if (!m) m = AllocVec(bytes + 15, MEMF_ANY | MEMF_CLEAR);
    *block = m;
    return m ? (APTR)(((ULONG)m + 15) & ~15UL) : NULL;
}

static void make_picture(ULONG *p, LONG w, LONG h, int alpha)
{
    LONG x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            ULONG r = (ULONG)(x * 255 / (w > 1 ? w - 1 : 1)), g = (ULONG)(y * 255 / (h > 1 ? h - 1 : 1)), b = (ULONG)((x ^ y) & 255);
            ULONG a = alpha ? (ULONG)((x * 7 + y * 3) % 256) : 0;   /* 24-bit pictures come with alpha 0 */
            p[y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
}

static void make_gradient(ULONG *g, LONG W, LONG H)
{
    LONG x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) g[y * W + x] = 0xff000000UL | ((ULONG)(40 + y * 120 / H) << 16) | ((ULONG)(60 + y * 100 / H) << 8) | 90;
}

/* Kinds 1, 2, 3 and 7: OpenBench's backdrop. */
static int backdrop_kind(struct Library *omc, const char *name, LONG sw, LONG sh, LONG W, LONG H, int alpha, ULONG reps, int quick)
{
    APTR sb = NULL, gb = NULL, db = NULL;
    ULONG *src, *grad, *dst = NULL;
    struct backdrop b;
    struct kind k;
    int bad = 0, inplace = sw == W && sh == H;
    src = alloc_line((ULONG)sw * sh * 4, &sb);
    grad = alloc_line((ULONG)W * H * 4, &gb);
    dst = alloc_line((ULONG)W * H * 4, &db);
    if (!src || !grad || !dst) {
        Printf((STRPTR)"%-11s no memory\n", (LONG)name);
        bad = 1;
        goto out;
    }
    make_picture(src, sw, sh, alpha);
    make_gradient(grad, W, H);
    b.src = inplace ? dst : src;
    b.sw = sw; b.sh = sh; b.grad = grad; b.dst = dst; b.W = W; b.H = H;
    b.opaque = alpha ? 0 : 0xff000000UL;
    if (W * sh > H * sw) { b.vw = sw; b.vh = H * sw / W; } else { b.vh = sh; b.vw = W * sh / H; }
    b.ox = (sw - b.vw) / 2;
    b.oy = (sh - b.vh) / 2;
    k.name = name; k.fn = backdrop_rows; k.data = &b; k.rows = (ULONG)H; k.stride = (ULONG)W * 4;
    k.out = dst; k.words = (ULONG)W * H; k.fresh = inplace ? src : NULL;
    bad = bench(omc, &k, reps, quick);
out:
    if (sb) FreeVec(sb);
    if (gb) FreeVec(gb);
    if (db) FreeVec(db);
    return bad;
}

/* Kinds 4, 5 and 6: openpng.datatype's colours. */
static int png_kind(struct Library *omc, const char *name, ULONG w, ULONG h, ULONG bpp, ULONG interlace, ULONG reps, int quick)
{
    APTR rb_ = NULL, ab = NULL;
    UBYTE *raw;
    ULONG *argb, len = 0, i;
    struct pngwork p;
    struct kind k;
    LONG pass;
    int bad = 0;
    p.w = w; p.h = h; p.bpp = bpp; p.interlace = interlace;
    for (pass = 0; pass < (interlace ? 7 : 1); pass++) {
        ULONG pw, ph;
        pass_size(&p, pass, &pw, &ph);
        len += ph * (pw * bpp + 1);
    }
    raw = alloc_line(len, &rb_);
    argb = alloc_line(w * h * 4, &ab);
    if (!raw || !argb) {
        Printf((STRPTR)"%-11s no memory\n", (LONG)name);
        bad = 1;
        goto out;
    }
    for (i = 0; i < len; i++) raw[i] = (UBYTE)((i * 2654435761UL) >> 24);   /* filter bytes too: png_rows skips them */
    p.raw = raw; p.got = len; p.argb = argb;
    k.name = name; k.fn = png_rows; k.data = &p; k.rows = h; k.stride = w * 4;
    k.out = argb; k.words = w * h; k.fresh = NULL;
    bad = bench(omc, &k, reps, quick);
out:
    if (rb_) FreeVec(rb_);
    if (ab) FreeVec(ab);
    return bad;
}

int main(void)
{
    LONG argv[3] = { 0, 0, 0 };
    struct RDArgs *ra = ReadArgs((STRPTR)"REPS/K/N,KIND/K/N,QUICK/S", argv, NULL);
    ULONG reps = 5, only = 0;
    int quick = 0, bad = 0;
    struct Library *omc;
    if (ra) {
        if (argv[0]) reps = *(LONG *)argv[0];
        if (argv[1]) only = *(LONG *)argv[1];
        quick = argv[2] != 0;
        FreeArgs(ra);
    } else {
        PutStr((STRPTR)"BandBench [REPS n] [KIND 1-7] [QUICK]\n");
        return 20;
    }
    if (!reps) reps = 1;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        PutStr((STRPTR)"BandBench: no timer.device\n");
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    {
        struct EClockVal e;
        efreq = ReadEClock(&e);
    }
    omc = OpenLibrary((STRPTR)OPENMULTICORE_NAME, 0);
    if (omc) {
        struct Library *OpenMulticoreBase = omc;
        struct OMCCoreInfo ci;
        cores = OMC_CoreCount();
        Printf((STRPTR)"BandBench: openmulticore.library %ld.%ld, %ld cores on the board",
               (LONG)omc->lib_Version, (LONG)omc->lib_Revision, (LONG)cores);
        if (cores && OMC_CoreInfo(1, &ci))
            Printf((STRPTR)" (core 1: %lx, engine %ld: 1 interpreter, 2 translated)", ci.oci_Model, ci.oci_Engine);
        PutStr((STRPTR)"\n");
    } else
        PutStr((STRPTR)"BandBench: no openmulticore.library: way 0 only\n");
    Printf((STRPTR)"ms, best of %ld; columns: 0 cores (plain), the library with no board, 1 2 4 8 cores\n", (LONG)reps);
    Printf((STRPTR)"%-11s%8s%8s%8s%8s%8s%8s\n", (LONG)"kind", (LONG)"0", (LONG)"cpu0", (LONG)"1", (LONG)"2", (LONG)"4", (LONG)"8");
    if (!only || only == 1) bad |= backdrop_kind(omc, "cover", 1600, 1200, 1920, 1080, 0, reps, quick);
    if (!only || only == 2) bad |= backdrop_kind(omc, "over", 1920, 1080, 1920, 1080, 1, reps, quick);
    if (!only || only == 3) bad |= backdrop_kind(omc, "cover+over", 1600, 1200, 1920, 1080, 1, reps, quick);
    if (!only || only == 4) bad |= png_kind(omc, "png rgb", 1920, 1080, 3, 0, reps, quick);
    if (!only || only == 5) bad |= png_kind(omc, "png rgba", 1920, 1080, 4, 0, reps, quick);
    if (!only || only == 6) bad |= png_kind(omc, "adam7", 1920, 1080, 4, 1, reps, quick);
    if (!only || only == 7) bad |= backdrop_kind(omc, "small", 64, 64, 64, 64, 1, reps, quick);
    if (omc) CloseLibrary(omc);
    CloseDevice((struct IORequest *)&treq);
    PutStr((STRPTR)(bad ? "BandBench: FAIL\n" : "BandBench: every way the same\n"));
    return bad ? 10 : 0;
}
