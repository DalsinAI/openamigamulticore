/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * OMCBench: what work is worth sending to a cores board, measured
 * (DESIGN.md section 5, W3). Four tables:
 *   1. the cost of an empty job: on the main CPU; on a board, one at a time
 *      and many in flight; what 0.2 added to every job (openservice.device
 *      opened, OPEN, WHERE, CLOSE, closed again); an empty RANGE index;
 *   2. break-even: a job of n turns of a loop on the main CPU against one
 *      board core, n from 0 to 1,000,000;
 *   3. throughput: ITEMS work items on the main CPU, as separate jobs, and
 *      as one range on 1, 2, 4 ... cores (ENV:OpenMulticore/RangeCores,
 *      read by the library when it makes its connection);
 *   4. the main CPU while every core works: a loop's speed against idle.
 * Times by the E-clock (timer.device). STRICT: every job strict (it reads
 * only its grants, this program's hunks among them), as over the LAN;
 * otherwise permissive (any Fast RAM and ROM), the library's default.
 *   OMCBench [JOBS n] [ITEMS n] [WORK n] [STRICT]
 * MIT, Copyright (c) 2026 Dalsin Limited. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/openmulticore.h>
#include <devices/openservice.h>   /* openamigaservice's include/: 0.2's per-job connection, timed alone */

struct Library *OpenMulticoreBase;
struct Device *TimerBase;
static struct timerequest treq;

typedef unsigned long long u64;
static ULONG efreq;
static u64 ticks(void) { struct EClockVal ev; efreq = ReadEClock(&ev); return (u64)ev.ev_hi << 32 | ev.ev_lo; }
static ULONG us(u64 t0, u64 t1) { return (ULONG)((t1 - t0) * 1000000ULL / efreq); }   /* microseconds */

/* The workers: no OS, their arguments and grants only. */
__attribute__((noinline)) LONG empty_job(void) { return 0; }
__attribute__((noinline)) LONG empty_index(ULONG index) { return (LONG)index; }
__attribute__((noinline)) LONG work_index(ULONG index, ULONG n, ULONG *out)
{
    ULONG k = index;
    for (ULONG i = 0; i < n; i++) k = k * 1103515245UL + 12345UL;
    out[index] = k;
    return (LONG)k;
}

static ULONG units, strict;
static BPTR program;                        /* STRICT: this program's hunks, granted to every job */

static void reopen(void)                    /* drops the library's connection (when this is its only user), so it reads RangeCores again */
{
    CloseLibrary(OpenMulticoreBase);
    OpenMulticoreBase = OpenLibrary((STRPTR)OPENMULTICORE_NAME, 0);
}
static void range_cores(ULONG n)            /* 0: unset */
{
    char buf[12];
    if (!n) DeleteVar((STRPTR)"OpenMulticore/RangeCores", GVF_GLOBAL_ONLY);
    else { int i = 11; buf[i] = 0; do buf[--i] = (char)('0' + n % 10); while ((n /= 10)); SetVar((STRPTR)"OpenMulticore/RangeCores", (STRPTR)buf + i, -1, GVF_GLOBAL_ONLY); }
    reopen();
}

static void job_for(struct OMCJob *j, APTR fn, ULONG target, ULONG flags)
{
    OMC_JobInit(j);
    j->omj_Entry = fn; j->omj_Target = target; j->omj_Flags = flags;
    j->omj_StackSize = 4096;
    if (strict) { j->omj_Flags |= OMCF_STRICT; OMC_GrantSeg(j, program, 0); }   /* reads: its code and its grants only */
}

/* 1. empty jobs: microseconds each, as tenths */
static void empty_costs(ULONG jobs)
{
    struct OMCJob j;
    u64 t0;
    ULONG cpu0, board = 0, flight = 0, conn = 0, index = 0, failed;
    job_for(&j, (APTR)empty_job, OMC_CPU0, 0);
    t0 = ticks();
    for (ULONG i = 0; i < jobs; i++) OMC_Run68k(&j);
    cpu0 = us(t0, ticks());
    if (units) {
        job_for(&j, (APTR)empty_job, OMC_ANY, OMCF_BOARD);
        OMC_Run68k(&j);                                   /* the connection made, the code translated */
        t0 = ticks();
        for (ULONG i = 0; i < jobs; i++) OMC_Run68k(&j);
        board = us(t0, ticks());
        {   /* 16 in flight, one signal bit */
            static struct OMCJob m[16];
            BYTE sig = AllocSignal(-1);
            ULONG sent = 0, done = 0;
            UBYTE busy[16] = { 0 };
            if (sig >= 0) {
                SetSignal(0, 1UL << sig);
                t0 = ticks();
                while (done < jobs) {
                    for (int k = 0; k < 16 && sent < jobs; k++)
                        if (!busy[k]) { job_for(&m[k], (APTR)empty_job, OMC_ANY, OMCF_BOARD); OMC_SetSignal(&m[k], NULL, 1UL << sig); OMC_Submit(&m[k]); busy[k] = 1; sent++; }
                    Wait(1UL << sig);
                    for (int k = 0; k < 16; k++) if (busy[k] && OMC_Check(&m[k])) { OMC_Wait(&m[k]); busy[k] = 0; done++; }
                }
                flight = us(t0, ticks());
                FreeSignal(sig);
            }
        }
        {   /* what 0.2 did around every job, alone */
            ULONG n = jobs / 10 ? jobs / 10 : 1;
            t0 = ticks();
            for (ULONG i = 0; i < n; i++) {
                struct MsgPort *port = CreateMsgPort();
                struct OSRequest *io = port ? (struct OSRequest *)CreateIORequest(port, sizeof *io) : NULL;
                if (io && !OpenDevice((STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)io, 0)) {
                    static const char name[] = "cpu.m68k/1";
                    for (int b = 0; b < 4; b++) { io->os_Buf[b].ob_Data = NULL; io->os_Buf[b].ob_Length = 0; }
                    io->os_Buf[0].ob_Data = (APTR)name; io->os_Buf[0].ob_Length = sizeof name - 1;
                    io->os_Flags = 0;
                    io->os_Req.io_Command = OSCMD_OPEN; DoIO((struct IORequest *)io);
                    io->os_Service = (UWORD)io->os_Result;
                    io->os_Req.io_Command = OSCMD_WHERE; DoIO((struct IORequest *)io);
                    io->os_Req.io_Command = OSCMD_CLOSE; DoIO((struct IORequest *)io);
                    CloseDevice((struct IORequest *)io);
                }
                if (io) DeleteIORequest((struct IORequest *)io);
                if (port) DeleteMsgPort(port);
            }
            conn = us(t0, ticks()) / n * jobs;            /* as if around every one of the jobs */
        }
    }
    job_for(&j, (APTR)empty_index, OMC_ANY, 0);
    OMC_RunRange(&j, 16, 0, &failed);
    t0 = ticks();
    OMC_RunRange(&j, jobs, 0, &failed);
    index = us(t0, ticks());
    Printf((STRPTR)"1. An empty job, microseconds (%ld of each; E-clock %ld Hz)\n", jobs, efreq);
    Printf((STRPTR)"   main CPU (OMC_CPU0)                      %6ld.%ld\n", cpu0 / jobs, cpu0 * 10 / jobs % 10);
    if (units) {
        Printf((STRPTR)"   board, one at a time (0.3)               %6ld.%ld\n", board / jobs, board * 10 / jobs % 10);
        Printf((STRPTR)"   board, 16 in flight (0.3)                %6ld.%ld\n", flight / jobs, flight * 10 / jobs % 10);
        Printf((STRPTR)"   0.2's connection per job, on top         %6ld.%ld\n", conn / jobs, conn * 10 / jobs % 10);
    }
    Printf((STRPTR)"   one RANGE index (%s)                %6ld.%ld\n", (LONG)(j.omj_Where == OMCW_BOARD ? "board" : "CPU0 "), index / jobs, index * 10 / jobs % 10);
}

/* 2. break-even: one job of n turns, main CPU against a board core */
static void break_even(ULONG *out)
{
    static const ULONG ns[] = { 0, 1000, 10000, 100000, 1000000 };
    struct OMCJob j;
    ULONG a[3];
    Printf((STRPTR)"\n2. One job of n turns of a loop, microseconds: main CPU against one board core\n");
    Printf((STRPTR)"   %9s %12s %12s\n", (LONG)"n", (LONG)"main CPU", (LONG)"board core");
    for (int q = 0; q < 5; q++) {
        ULONG n = ns[q], reps = n <= 1000 ? 200 : n <= 10000 ? 50 : n <= 100000 ? 10 : 3, tc, tb = 0;
        u64 t0;
        a[0] = 0; a[1] = n; a[2] = (ULONG)out;
        job_for(&j, (APTR)work_index, OMC_CPU0, 0);
        j.omj_Args = a; j.omj_NArgs = 3;
        OMC_AddGrant(&j, out, 16, OMCG_READ | OMCG_WRITE);
        t0 = ticks();
        for (ULONG r = 0; r < reps; r++) OMC_Run68k(&j);
        tc = us(t0, ticks()) / reps;
        if (units) {
            j.omj_Target = OMC_ANY; j.omj_Flags |= OMCF_BOARD;
            OMC_Run68k(&j);
            t0 = ticks();
            for (ULONG r = 0; r < reps; r++) OMC_Run68k(&j);
            tb = us(t0, ticks()) / reps;
        }
        Printf((STRPTR)"   %9ld %12ld %12ld\n", n, tc, tb);
    }
}

/* ITEMS work items as separate jobs, all in flight on one signal: microseconds */
static ULONG as_jobs(ULONG items, ULONG work, ULONG *out)
{
    static struct OMCJob m[256];
    static ULONG a[256][3];
    UBYTE done[256];
    BYTE sig = AllocSignal(-1);
    ULONG left = items;
    u64 t0;
    if (sig < 0) return 0;
    SetSignal(0, 1UL << sig);
    t0 = ticks();
    for (ULONG i = 0; i < items; i++) {                    /* item i into its own 16-byte line */
        done[i] = 0;
        job_for(&m[i], (APTR)work_index, OMC_ANY, 0);
        a[i][0] = i; a[i][1] = work; a[i][2] = (ULONG)(out + 3 * i);   /* so out[index] is out[4i] */
        m[i].omj_Args = a[i]; m[i].omj_NArgs = 3;
        OMC_AddGrant(&m[i], out + 4 * i, 16, OMCG_READ | OMCG_WRITE);
        OMC_SetSignal(&m[i], NULL, 1UL << sig);
        OMC_Submit(&m[i]);
    }
    while (left) {
        Wait(1UL << sig);
        for (ULONG i = 0; i < items; i++) if (!done[i] && OMC_Check(&m[i])) { OMC_Wait(&m[i]); done[i] = 1; left--; }
    }
    FreeSignal(sig);
    return us(t0, ticks());
}
static ULONG as_range(ULONG items, ULONG work, ULONG *out, ULONG *where)
{
    struct OMCJob j;
    ULONG a[2] = { work, (ULONG)out }, failed;
    u64 t0;
    job_for(&j, (APTR)work_index, OMC_ANY, 0);
    j.omj_Args = a; j.omj_NArgs = 2;
    OMC_AddGrant(&j, out, OMC_GRANTSIZE(items * 4), OMCG_READ | OMCG_WRITE);
    t0 = ticks();
    OMC_RunRange(&j, items, 4, &failed);
    if (where) *where = j.omj_Where;
    return failed == items ? us(t0, ticks()) : 0;
}

static void ratio(ULONG base, ULONG t) { if (t) Printf((STRPTR)"%5ld.%02ldx", base / t, base * 100 / t % 100); else PutStr((STRPTR)"     -"); }

/* 3. throughput */
static void throughput(ULONG items, ULONG work, ULONG *out)
{
    ULONG t0us, tj, tr, where = 0;
    u64 t0 = ticks();
    for (ULONG i = 0; i < items; i++) work_index(i, work, out);
    t0us = us(t0, ticks());
    tj = units ? as_jobs(items, work, out) : 0;
    tr = as_range(items, work, out, &where);
    Printf((STRPTR)"\n3. %ld work items of %ld turns, milliseconds (speed against the main CPU)\n", items, work);
    Printf((STRPTR)"   main CPU, plain calls        %8ld\n", t0us / 1000);
    if (units) { Printf((STRPTR)"   separate jobs, all at once   %8ld  ", tj / 1000); ratio(t0us, tj); PutStr((STRPTR)"\n"); }
    Printf((STRPTR)"   one range, every core (%s) %6ld  ", (LONG)(where == OMCW_BOARD ? "board" : "CPU0 "), tr / 1000); ratio(t0us, tr); PutStr((STRPTR)"\n");
    for (ULONG n = 1; units && n <= 32; n = n < 2 ? 2 : n + (n < 8 ? 2 : 4)) {   /* 1, 2, 4, 6, 8, 12 ... and the board's own count last */
        ULONG c = n < units ? n : units;
        range_cores(c);
        if (!OpenMulticoreBase) return;
        tr = as_range(items, work, out, NULL);
        Printf((STRPTR)"   one range, %2ld core%s           %8ld  ", c, (LONG)(c == 1 ? " " : "s"), tr / 1000); ratio(t0us, tr); PutStr((STRPTR)"\n");
        if (c == units) break;
    }
    if (units) range_cores(0);
}

/* 4. the main CPU's own speed while every core works */
static ULONG spin_for(ULONG ms)
{
    volatile ULONG n = 0;
    u64 t0 = ticks(), end = t0 + (u64)efreq * ms / 1000;
    while (ticks() < end) for (int i = 0; i < 1000; i++) n++;
    return n / 1000;
}
static void responsiveness(ULONG *out)
{
    struct OMCJob j;
    ULONG a[2] = { 50000000UL, (ULONG)out }, failed, idle, busy;
    if (!units) return;
    idle = spin_for(1000);
    job_for(&j, (APTR)work_index, OMC_ANY, OMCF_BOARD);
    j.omj_Args = a; j.omj_NArgs = 2; j.omj_TimeoutMS = 600000;
    OMC_AddGrant(&j, out, OMC_GRANTSIZE(units * 4 * 4), OMCG_READ | OMCG_WRITE);
    if (OMC_SubmitRange(&j, units * 4, 4, &failed) != OMCERR_OK) { PutStr((STRPTR)"\n4. the range was refused\n"); return; }
    Delay(10);
    busy = spin_for(1000);
    OMC_Abort(&j);
    OMC_Wait(&j);
    Printf((STRPTR)"\n4. The main CPU's loop, thousands of turns a second: alone %ld, with every core busy %ld (%ld%%)\n",
           idle, busy, idle ? busy * 100 / idle : 0);
}

int main(void)
{
    LONG a[4] = { 0, 0, 0, 0 };
    struct RDArgs *rd = ReadArgs((STRPTR)"JOBS/K/N,ITEMS/K/N,WORK/K/N,STRICT/S", a, NULL);
    ULONG jobs = a[0] ? *(ULONG *)a[0] : 2000, items = a[1] ? *(ULONG *)a[1] : 64, work = a[2] ? *(ULONG *)a[2] : 2000000;
    ULONG *out;
    if (rd) FreeArgs(rd);
    if (items > 256) items = 256;
    strict = a[3] != 0;
    {
        struct CommandLineInterface *cli = BADDR(((struct Process *)FindTask(NULL))->pr_CLI);
        program = cli ? cli->cli_Module : 0;
        if (strict && !program) { PutStr((STRPTR)"STRICT needs to run from a shell\n"); return 20; }
    }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) { PutStr((STRPTR)"no timer.device\n"); return 20; }
    TimerBase = treq.tr_node.io_Device;
    if (!(OpenMulticoreBase = OpenLibrary((STRPTR)OPENMULTICORE_NAME, 0)) || !OMC_HAS_RANGE(OpenMulticoreBase)) {
        PutStr((STRPTR)"OMCBench needs openmulticore.library 0.3\n");
        if (OpenMulticoreBase) CloseLibrary(OpenMulticoreBase);
        CloseDevice((struct IORequest *)&treq);
        return 20;
    }
    if (!(out = OMC_AllocGrant(256 * 16, OMCAF_CLEAR | OMCAF_PAGE))) return 20;   /* pages of its own, apart from the code */
    units = OMC_CoreCount();
    Printf((STRPTR)"OMCBench: openmulticore.library %ld.%ld, %ld cores on the board, %s jobs\n\n", (LONG)OpenMulticoreBase->lib_Version,
           (LONG)OpenMulticoreBase->lib_Revision, units, (LONG)(strict ? "strict" : "permissive"));
    empty_costs(jobs);
    break_even(out);
    throughput(items, work, out);
    if (OpenMulticoreBase) responsiveness(out);
    OMC_FreeGrant(out);
    if (OpenMulticoreBase) CloseLibrary(OpenMulticoreBase);
    CloseDevice((struct IORequest *)&treq);
    return 0;
}
