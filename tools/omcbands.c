/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * OMCBands: omc_bands() (include/openmulticore/bands.h) tested and timed.
 * Each workload is a loop over rows of a 32-bit picture, run three ways:
 *   plain   the band function called once for every row (what a machine
 *           without openmulticore.library does: OMCBF_LOCAL);
 *   cpu0    the bands through the library aimed at the main CPU (what the
 *           library does with no cores board: OMCBF_LIBCPU0);
 *   cores   the bands on the cores board, the caller taking a band too.
 * Every way must give the same bytes; the times are the best of REPS runs,
 * in milliseconds from the E clock.
 *
 * The workloads are OpenBench's backdrop at boot (openamigabench
 * desktop/openbench.c, cover() and over_gradient()): a 1600 x 1200 picture
 * with alpha cut and scaled to cover 1920 x 1080 and laid over the theme's
 * gradient; the same without alpha (24-bit, as the OpenUp backdrops are);
 * a screen-sized picture with alpha laid over it in place; a screen-sized
 * 24-bit one (Instance-32's boot: the backdrop's screen-sized twin); and a
 * 64 x 64 one, to show what a call costs when there is almost nothing to share.
 *   OMCBands [REPS n] [W n] [H n] [CORES n] [WORK n]
 * CORES: at most n bands to the cores; WORK: only workload n (1 to 5).
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

static ULONG now_us(void)
{
    struct EClockVal e;
    unsigned long long t;
    ReadEClock(&e);
    t = ((unsigned long long)e.ev_hi << 32) | e.ev_lo;
    return (ULONG)(t * 1000000ULL / efreq);
}

/* ---- the workloads: band functions, jobs by the rules (no OS, no globals written) ---- */

struct backdrop {
    const ULONG *src;               /* the picture, sw x sh */
    LONG sw, sh, vw, vh, ox, oy;    /* the part of it that covers the screen */
    const ULONG *grad;              /* the gradient, W x H */
    ULONG *dst;                     /* W x H */
    LONG W, H;
    ULONG opaque;                   /* 0xff000000 for a picture without alpha, else 0 */
};

/* OpenBench 0.1.15's backdrop_rows() (openamigabench desktop/openbench.c), the same code: rows y0 to
 * y1 - 1 of the picture cut and scaled to cover the screen, made opaque or laid over the gradient. */
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

/* ---- the test ---------------------------------------------------------------------------- */

static ULONG sum(const ULONG *p, ULONG n)
{
    ULONG h = 2166136261UL;
    while (n--) h = (h ^ *p++) * 16777619UL;
    return h;
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

/* A buffer on a 16-byte line of its own (bands.h), with its block for FreeVec. */
static ULONG *alloc_line(ULONG bytes, APTR *block)
{
    UBYTE *m = AllocVec(bytes + 15, MEMF_FAST | MEMF_CLEAR);
    if (!m) m = AllocVec(bytes + 15, MEMF_ANY | MEMF_CLEAR);
    *block = m;
    return m ? (ULONG *)(((ULONG)m + 15) & ~15UL) : NULL;
}

static const char *const way_name[3] = { "plain", "cpu0 ", "cores" };
static const ULONG way_flags[3] = { OMCBF_LOCAL, OMCBF_LIBCPU0, 0 };

/* One workload: each way REPS times, the picture made afresh before every run (the work is in place
 * when the picture is screen sized). 0 when every way gave the same bytes. */
static ULONG max_cores;     /* CORES n: at most n bands to the cores */

static int workload(struct Library *omc, const char *name, LONG sw, LONG sh, LONG W, LONG H, int alpha, ULONG reps)
{
    APTR sb = NULL, gb = NULL, db = NULL, pb = NULL;
    ULONG *src, *grad, *dst, *pic = NULL, want = 0, w, r;
    struct backdrop b;
    int bad = 0;
    src = alloc_line((ULONG)sw * sh * 4, &sb);
    grad = alloc_line((ULONG)W * H * 4, &gb);
    dst = alloc_line((ULONG)W * H * 4, &db);
    if (sw == W && sh == H) pic = alloc_line((ULONG)W * H * 4, &pb);
    if (!src || !grad || !dst || (sw == W && sh == H && !pic)) {
        Printf((STRPTR)"%s: no memory\n", (LONG)name);
        bad = 1;
        goto out;
    }
    make_picture(src, sw, sh, alpha);
    {
        LONG y, x;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) grad[y * W + x] = 0xff000000UL | ((ULONG)(40 + y * 120 / H) << 16) | ((ULONG)(60 + y * 100 / H) << 8) | 90;
    }
    b.src = src; b.sw = sw; b.sh = sh; b.grad = grad; b.W = W; b.H = H; b.opaque = alpha ? 0 : 0xff000000UL;
    if (W * sh > H * sw) { b.vw = sw; b.vh = H * sw / W; } else { b.vh = sh; b.vw = W * sh / H; }
    b.ox = (sw - b.vw) / 2;
    b.oy = (sh - b.vh) / 2;
    Printf((STRPTR)"%s: %ldx%ld%s -> %ldx%ld\n", (LONG)name, sw, sh, (LONG)(alpha ? " with alpha" : ""), W, H);
    for (w = 0; w < 3; w++) {
        ULONG best = ~0UL, total = 0, got = 0;
        struct OMCBandStats st;
        if (w > 0 && !omc) { Printf((STRPTR)"  %s  (no openmulticore.library)\n", (LONG)way_name[w]); continue; }
        for (r = 0; r < reps; r++) {
            ULONG t0, t1;
            if (pic) { CopyMemQuick(src, pic, (ULONG)W * H * 4); b.src = b.dst = pic; }   /* in place, as OpenBench */
            else { b.dst = dst; }
            t0 = now_us();
            omc_bands(omc, backdrop_rows, &b, (ULONG)H, b.dst, (ULONG)W * 4, way_flags[w] | OMCBF_MAXCORES(max_cores), &st);
            t1 = now_us();
            if (t1 - t0 < best) best = t1 - t0;
            total += t1 - t0;
            got = sum(b.dst, (ULONG)W * H);
            if (w == 0 && r == 0) want = got;
            else if (got != want) bad = 1;
        }
        Printf((STRPTR)"  %s  best %5ld.%ld ms  mean %5ld.%ld ms  bands %ld, on the board %ld, redone %ld, caller's rows %ld  sum %08lx %s\n",
               (LONG)way_name[w], best / 1000, (best % 1000) / 100, total / reps / 1000, (total / reps % 1000) / 100,
               st.obs_Bands, st.obs_Board, st.obs_Redone, st.obs_SelfRows, got, (LONG)(got == want ? "ok" : "DIFFERENT"));
    }
out:
    if (sb) FreeVec(sb);
    if (gb) FreeVec(gb);
    if (db) FreeVec(db);
    if (pb) FreeVec(pb);
    return bad;
}

int main(void)
{
    LONG argv[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *ra = ReadArgs((STRPTR)"REPS/K/N,W/K/N,H/K/N,CORES/K/N,WORK/K/N", argv, NULL);
    ULONG reps = 5, W = 1920, H = 1080, only = 0;
    struct Library *omc;
    int bad = 0;
    if (ra) {
        if (argv[0]) reps = *(LONG *)argv[0];
        if (argv[1]) W = *(LONG *)argv[1];
        if (argv[2]) H = *(LONG *)argv[2];
        if (argv[3]) max_cores = *(LONG *)argv[3];
        if (argv[4]) only = *(LONG *)argv[4];
        FreeArgs(ra);
    }
    if (!reps) reps = 1;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        PutStr((STRPTR)"OMCBands: no timer.device\n");
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
        ULONG n = OMC_CoreCount();
        Printf((STRPTR)"OMCBands: openmulticore.library %ld.%ld, %ld cores%s\n", (LONG)omc->lib_Version, (LONG)omc->lib_Revision,
               (LONG)n, (LONG)(max_cores ? ", bands to the cores limited" : ""));
        if (n && OMC_CoreInfo(1, &ci))
            Printf((STRPTR)"  core 1: model %lx, engine %ld (1 interpreter, 2 translated), host CPU %ld, jobs done %ld\n",
                   ci.oci_Model, ci.oci_Engine, (LONG)ci.oci_HostCPU, ci.oci_JobsDone);
    }
    else
        PutStr((STRPTR)"OMCBands: no openmulticore.library (plain calls only)\n");
    if (!only || only == 1) bad |= workload(omc, "scale+blend", 1600, 1200, (LONG)W, (LONG)H, 1, reps);
    if (!only || only == 2) bad |= workload(omc, "scale 24-bit", 1600, 1200, (LONG)W, (LONG)H, 0, reps);
    if (!only || only == 3) bad |= workload(omc, "blend in place", (LONG)W, (LONG)H, (LONG)W, (LONG)H, 1, reps);
    if (!only || only == 4) bad |= workload(omc, "24-bit in place", (LONG)W, (LONG)H, (LONG)W, (LONG)H, 0, reps);
    if (!only || only == 5) bad |= workload(omc, "small", 64, 64, 64, 64, 1, reps);
    if (omc) CloseLibrary(omc);
    CloseDevice((struct IORequest *)&treq);
    PutStr((STRPTR)(bad ? "OMCBands: FAIL\n" : "OMCBands: all ways the same\n"));
    return bad ? 10 : 0;
}
