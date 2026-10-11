/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * omc_bands(): a loop over rows split across the cores, header-only.
 *
 *   static void band(APTR data, ULONG y0, ULONG y1) { ...rows y0 to y1 - 1... }
 *   omc_bands(NULL, band, &args, rows, out, stride, 0, NULL);
 *
 * The rows are cut into bands, one for each core of a cores board and one
 * for the caller, which runs its own band while the cores run theirs, then
 * collects them. With no openmulticore.library, no board, or a picture too
 * small to share, every row runs on the caller as a plain call: the same
 * function, the same answer. A band that comes back with any error (a fault,
 * a timeout, Chip RAM, no memory) is run again on the caller, so the answer
 * is always whole.
 *
 * The band function is a job (DESIGN.md section 4, CORES_BOARD.md section 1):
 *  - no OS calls, no malloc, printf or float library calls; integer code
 *    and its own helpers only (they run where it runs);
 *  - it writes only rows y0 to y1 - 1 of out (row y starts at
 *    out + y * stride), and its stack;
 *  - it reads anything it likes in Fast RAM (data, tables, its own code,
 *    the rows it writes), never Chip RAM;
 *  - it gives the same bytes when run twice (a band may be run again).
 * Bands that go to a board have their rows granted written, which needs
 * each band to start and end on a 16-byte line: the cut points are chosen
 * so, and rows before the first such line or after the last go to the
 * caller's share. So give out a 16-byte line of its own to start on
 * (OMC_AllocGrant, or AllocVec with 15 bytes more, the pointer rounded up):
 * AllocVec's own memory is only longword aligned, and with a stride that is
 * a multiple of 16 no row of it would start on a line, which leaves every
 * row to the caller.
 *
 * Each call costs some setup on the caller for every band it sends (the
 * board's service opened, the job written): measured on Instance-32 in
 * tools/omcbands.c. Leave small pictures to a plain call.
 *
 * base: openmulticore.library's base, or NULL to open it here (and close it
 * again). st: what happened, or NULL. 0 always (the work is done).
 *
 * Built with bebbo's m68k-amigaos-gcc; needs SysBase. 11 October 2026, the Team. */
#ifndef OPENMULTICORE_BANDS_H
#define OPENMULTICORE_BANDS_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <libraries/openmulticore.h>
#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif
#include <inline/openmulticore.h>

typedef void (*omc_band_fn)(APTR data, ULONG y0, ULONG y1);

#define OMC_BANDS_MAX 8                 /* jobs at once: the cores board's most */

/* omc_bands()' flags */
#define OMCBF_LOCAL   (1UL << 0)        /* every row on the caller: what a machine without the library does */
#define OMCBF_LIBCPU0 (1UL << 1)        /* the bands go through the library aimed at the main CPU: the library's fallback, for tests */
#define OMCBF_NOSELF  (1UL << 2)        /* the caller takes no band of its own, only the rows the board can't */

struct OMCBandStats {
    ULONG obs_Bands;                    /* jobs sent */
    ULONG obs_Board;                    /* of them, run on a board's core */
    ULONG obs_Redone;                   /* run again on the caller after an error */
    ULONG obs_SelfRows;                 /* rows the caller ran itself */
};

static __attribute__((unused)) ULONG omc__align_up(ULONG y, ULONG ya, ULONG q, ULONG last)
{
    if (y <= ya) return ya;
    y = ya + ((y - ya + q - 1) / q) * q;
    return y < last ? y : last;
}

static __attribute__((unused)) LONG omc_bands(struct Library *base, omc_band_fn fn, APTR data, ULONG rows,
                                              APTR out, ULONG stride, ULONG flags, struct OMCBandStats *st)
{
    struct Library *OpenMulticoreBase = base;     /* the inline calls use this name */
    struct OMCJob *jobs = NULL;
    ULONG (*args)[3] = NULL;
    ULONG lo[OMC_BANDS_MAX], hi[OMC_BANDS_MAX], n = 0, k, cores, q, ya, last, share, self = 0, opened = 0;
    LONG sub[OMC_BANDS_MAX];
    BYTE sig = -1;
    struct OMCBandStats dummy;
    UBYTE *o = (UBYTE *)out;
    if (!st) st = &dummy;
    st->obs_Bands = st->obs_Board = st->obs_Redone = st->obs_SelfRows = 0;
    if (!rows) return 0;
    if (flags & OMCBF_LOCAL) goto local;
    if (!OpenMulticoreBase) {
        if (!(OpenMulticoreBase = OpenLibrary((CONST_STRPTR)OPENMULTICORE_NAME, 0))) goto local;
        opened = 1;
    }
    cores = OMC_CoreCount();
    if ((flags & OMCBF_LIBCPU0) && !cores) cores = 2;
    if (!cores) goto local;
    if (cores > OMC_BANDS_MAX) cores = OMC_BANDS_MAX;
    /* q rows make a whole number of 16-byte lines; row ya is the first on a line's start */
    for (q = 1; (q * stride) & 15; q++) ;
    for (ya = 0; ya < q && (((ULONG)o + ya * stride) & 15); ya++) ;
    if (ya >= q || ya >= rows) goto local;        /* no row starts on a line */
    last = ya + ((rows - ya) / q) * q;            /* the last line-aligned row; rows after it are the caller's */
    self = (flags & OMCBF_NOSELF) ? 0 : 1;
    share = rows / (cores + self);
    if (share < 2 * q) {                          /* too few rows for so many bands */
        cores = rows / (2 * q);
        if (cores < 2) goto local;
        cores -= self;
        if (cores > OMC_BANDS_MAX) cores = OMC_BANDS_MAX;
        if (!cores) goto local;
        share = rows / (cores + self);
    }
    {
        ULONG y = omc__align_up(self ? share : 0, ya, q, last);
        for (k = 0; k < cores && y < last; k++) {
            ULONG e = k + 1 == cores ? last : omc__align_up(y + share, ya, q, last);
            if (e <= y) continue;
            lo[n] = y; hi[n] = e; n++;
            y = e;
        }
    }
    if (!n) goto local;
    if (!(jobs = AllocVec(n * sizeof *jobs, MEMF_PUBLIC | MEMF_CLEAR)) || !(args = AllocVec(n * sizeof *args, MEMF_PUBLIC)))
        goto local_free;
    if (OMC_HAS_JOBCALLS(OpenMulticoreBase)) sig = AllocSignal(-1);
    CacheClearE(NULL, ~0UL, CACRF_ClearD);        /* what the cores read is in memory, not only in the cache */
    for (k = 0; k < n; k++) {
        struct OMCJob *j = &jobs[k];
        if (OMC_HAS_JOBCALLS(OpenMulticoreBase)) OMC_JobInit(j);
        else { j->omj_StackSize = OMC_DEFAULTSTACK; j->omj_TimeoutMS = OMC_DEFAULTTIMEOUT; }
        j->omj_Entry = (APTR)fn;
        args[k][0] = (ULONG)data; args[k][1] = lo[k]; args[k][2] = hi[k];
        j->omj_Args = args[k];
        j->omj_NArgs = 3;
        j->omj_Target = (flags & OMCBF_LIBCPU0) ? OMC_CPU0 : OMC_ANY;
        j->omj_TimeoutMS = 60000;
        j->omj_Grants[0].og_Addr = o + lo[k] * stride;
        j->omj_Grants[0].og_Length = (hi[k] - lo[k]) * stride;
        j->omj_Grants[0].og_Mode = OMCG_READ | OMCG_WRITE;
        j->omj_NGrants = 1;
        if (sig >= 0) OMC_SetSignal(j, NULL, 1UL << sig);
        sub[k] = OMC_Submit(j);                   /* on the main CPU (no board, or the user's choice) it has run now */
        st->obs_Bands++;
    }
    /* the caller's own rows, while the cores run theirs */
    if (lo[0] > 0) { fn(data, 0, lo[0]); st->obs_SelfRows += lo[0]; }
    if (hi[n - 1] < rows) { fn(data, hi[n - 1], rows); st->obs_SelfRows += rows - hi[n - 1]; }
    for (k = 0; k < n; k++) {
        struct OMCJob *j = &jobs[k];
        LONG r = sub[k] != OMCERR_OK ? sub[k] : OMC_Wait(j);
        if (r != OMCERR_OK) {
            fn(data, lo[k], hi[k]);               /* run again here: the rows are whole either way */
            st->obs_Redone++;
            st->obs_SelfRows += hi[k] - lo[k];
        } else {
            if (j->omj_Where == OMCW_BOARD) st->obs_Board++;
            CacheClearE(o + lo[k] * stride, (hi[k] - lo[k]) * stride, CACRF_ClearD);
        }
    }
    if (sig >= 0) { SetSignal(0, 1UL << sig); FreeSignal(sig); }   /* nothing left pending on a bit given back */
    FreeVec(args);
    FreeVec(jobs);
    if (opened) CloseLibrary(OpenMulticoreBase);
    return 0;
local_free:
    if (args) FreeVec(args);
    if (jobs) FreeVec(jobs);
local:
    if (opened) CloseLibrary(OpenMulticoreBase);
    fn(data, 0, rows);
    st->obs_SelfRows = rows;
    return 0;
}

#endif
