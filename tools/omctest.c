/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * OMCTest: openmulticore.library end to end. Lists the cores, then runs one
 * pure 68k function (it sums an array into a written buffer and returns
 * the total) on the main CPU, on any core, and on each named core, checking
 * every answer against the main CPU's and timing them; on a board it also
 * checks a fault comes back as one, and a strict job with its program's
 * hunks granted. With 0.2 it also checks OMC_JobInit's defaults,
 * OMC_AddGrant's refusals, OMC_AllocGrant memory in real jobs, OMC_Abort's
 * status, and a completion signal over more jobs at once than a task has
 * signal bits, on whatever cores there are and on the main CPU alone.
 * NOLIB (or no library) runs the port's fallback, plain calls, instead.
 *   OMCTest [COUNT n] [SIZE n] [NOLIB]
 * MIT, Copyright (c) 2026 Dalsin Limited. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/openmulticore.h>

struct Library *OpenMulticoreBase;

/* The job: no OS, its arguments and grants only. GCC passes the arguments on
 * the stack, as OMCJob's omj_Args. */
__attribute__((noinline)) LONG sum_job(const ULONG *data, ULONG n, ULONG *out)
{
    ULONG t = 0, x = 0;
    for (ULONG i = 0; i < n; i++) { t += data[i]; x ^= data[i] * 2654435761UL; }
    out[0] = t; out[1] = x;
    return (LONG)(t ^ x);
}

__attribute__((noinline)) LONG bad_job(void)
{
    __asm volatile ("illegal");
    return 0;
}

static ULONG *data, *out;   /* out in its own memory: a written buffer may not overlap the granted hunks */
static ULONG args[3];

static void setup(struct OMCJob *j, ULONG n, ULONG target)
{
    UBYTE *p = (UBYTE *)j;
    for (ULONG i = 0; i < sizeof *j; i++) p[i] = 0;
    j->omj_Entry = (APTR)sum_job;
    args[0] = (ULONG)data; args[1] = n; args[2] = (ULONG)out;
    j->omj_Args = args; j->omj_NArgs = 3;
    j->omj_Grants[0].og_Addr = data; j->omj_Grants[0].og_Length = n * 4; j->omj_Grants[0].og_Mode = OMCG_READ;
    j->omj_Grants[1].og_Addr = out; j->omj_Grants[1].og_Length = 8; j->omj_Grants[1].og_Mode = OMCG_READ | OMCG_WRITE;
    j->omj_NGrants = 2;
    j->omj_Target = target;
}

static const char *where(ULONG w) { return w == OMCW_CPU0 ? "CPU0" : w == OMCW_BOARD ? "board" : w == OMCW_LAN ? "LAN" : "-"; }

/* A job long enough to abort: n turns of a loop. */
__attribute__((noinline)) LONG spin_job(ULONG n)
{
    volatile ULONG k = 0;
    for (ULONG i = 0; i < n; i++) k += i;
    return (LONG)k;
}

static ULONG check(const char *what, LONG got, LONG wanted)
{
    if (got == wanted) return 0;
    Printf((STRPTR)"  FAIL %s: %ld, wanted %ld\n", (LONG)what, got, wanted);
    return 1;
}

static ULONG free_signals(void)
{
    ULONG a = ~FindTask(NULL)->tc_SigAlloc, n = 0;
    while (a) { n += a & 1; a >>= 1; }
    return n;
}

/* A job for sum_job with 0.2's calls: OMC_JobInit, OMC_AddGrant. */
static LONG setup2(struct OMCJob *j, ULONG *a3, const ULONG *d, ULONG n, ULONG *o, ULONG target)
{
    LONG r;
    OMC_JobInit(j);
    j->omj_Entry = (APTR)sum_job;
    a3[0] = (ULONG)d; a3[1] = n; a3[2] = (ULONG)o;
    j->omj_Args = a3; j->omj_NArgs = 3;
    j->omj_Target = target;
    if ((r = OMC_AddGrant(j, (APTR)d, n * 4, OMCG_READ))) return r;
    return OMC_AddGrant(j, o, OMC_GRANTSIZE(8), OMCG_READ | OMCG_WRITE);
}

#define SIGJOBS 24              /* more at once than a task has free signal bits */

/* 0.2's calls. The number of failures. */
static ULONG test_v02(ULONG units, ULONG size, ULONG want, LONG want_d0)
{
    static struct OMCJob jobs[SIGJOBS];
    static ULONG jargs[SIGJOBS][3];
    ULONG *outs[SIGJOBS];
    struct OMCJob job;
    ULONG bad = 0, a3[3];
    UBYTE *g1, *g2, *g3, *chip, *p;

    /* OMC_JobInit: everything zero but the defaults */
    p = (UBYTE *)&job;
    for (ULONG i = 0; i < sizeof job; i++) p[i] = 0xA5;
    OMC_JobInit(&job);
    {
        ULONG priv = 0;
        for (int i = 0; i < 12; i++) priv |= job.omj_Private[i];
        bad += check("JobInit target", job.omj_Target, OMC_ANY);
        bad += check("JobInit stack", job.omj_StackSize, OMC_DEFAULTSTACK);
        bad += check("JobInit timeout", job.omj_TimeoutMS, OMC_DEFAULTTIMEOUT);
        bad += check("JobInit grants, flags, entry, private", job.omj_NGrants | job.omj_Flags | (ULONG)job.omj_Entry | job.omj_NArgs | priv, 0);
    }
    Printf((STRPTR)"JobInit: target %lx, stack %ld, timeout %ld ms\n", job.omj_Target, job.omj_StackSize, job.omj_TimeoutMS);

    /* OMC_AllocGrant: aligned, cleared, Fast RAM when there is some */
    g1 = OMC_AllocGrant(1000, OMCAF_CLEAR);
    g2 = OMC_AllocGrant(64, 0);
    g3 = OMC_AllocGrant(100, OMCAF_PAGE | OMCAF_CLEAR);
    if (!g1 || !g2 || !g3) { PutStr((STRPTR)"  FAIL AllocGrant: no memory\n"); return bad + 1; }
    {
        ULONG nz = 0;
        for (ULONG i = 0; i < 1024; i++) nz |= g1[i];
        bad += check("AllocGrant 64-byte line", (ULONG)g1 & 63, 0);
        bad += check("AllocGrant 64-byte line (2)", (ULONG)g2 & 63, 0);
        bad += check("AllocGrant page", (ULONG)g3 & 4095, 0);
        bad += check("AllocGrant cleared", nz, 0);
        if (AvailMem(MEMF_FAST)) bad += check("AllocGrant in Fast RAM", (TypeOfMem(g1) & MEMF_FAST) != 0, 1);
    }
    Printf((STRPTR)"AllocGrant: %08lx %08lx %08lx (page)\n", (ULONG)g1, (ULONG)g2, (ULONG)g3);

    /* OMC_AddGrant's refusals, each leaving the job as it was */
    OMC_JobInit(&job);
    bad += check("AddGrant NULL", OMC_AddGrant(&job, NULL, 16, OMCG_READ), OMCERR_BADGRANT);
    bad += check("AddGrant length 0", OMC_AddGrant(&job, g1, 0, OMCG_READ), OMCERR_BADGRANT);
    bad += check("AddGrant no mode", OMC_AddGrant(&job, g1, 16, 0), OMCERR_BADGRANT);
    bad += check("AddGrant unknown mode", OMC_AddGrant(&job, g1, 16, 8), OMCERR_BADGRANT);
    bad += check("AddGrant written off a line", OMC_AddGrant(&job, g1 + 4, 16, OMCG_WRITE), OMCERR_BADGRANT);
    bad += check("AddGrant written part line", OMC_AddGrant(&job, g1, 20, OMCG_WRITE), OMCERR_BADGRANT);
    bad += check("AddGrant written 1", OMC_AddGrant(&job, g1, 512, OMCG_READ | OMCG_WRITE), OMCERR_OK);
    bad += check("AddGrant read over written", OMC_AddGrant(&job, g1 + 100, 8, OMCG_READ), OMCERR_BADGRANT);
    bad += check("AddGrant written 2", OMC_AddGrant(&job, g2, 64, OMCG_WRITE), OMCERR_OK);
    bad += check("AddGrant written 3", OMC_AddGrant(&job, g3, 64, OMCG_WRITE), OMCERR_NOSLOT);
    for (ULONG i = 0; job.omj_NGrants < OMC_MAXGRANTS; i++)
        bad += check("AddGrant read", OMC_AddGrant(&job, g3 + 64 * i, 64, OMCG_READ), OMCERR_OK);
    bad += check("AddGrant 17th", OMC_AddGrant(&job, g1 + 512, 16, OMCG_READ), OMCERR_NOSLOT);
    bad += check("AddGrant left the job", job.omj_NGrants, OMC_MAXGRANTS);
    if ((chip = AllocMem(64, MEMF_CHIP))) {
        OMC_JobInit(&job);
        bad += check("AddGrant Chip RAM, any core", OMC_AddGrant(&job, chip, 64, OMCG_READ), OMCERR_OK);
        bad += check("AddGrant Chip RAM keeps it on CPU0", (job.omj_Flags & OMCF_NOBOARD) != 0, 1);
        OMC_JobInit(&job); job.omj_Flags = OMCF_BOARD;
        bad += check("AddGrant Chip RAM, board only", OMC_AddGrant(&job, chip, 64, OMCG_READ), OMCERR_CHIPRAM);
        OMC_JobInit(&job); job.omj_Target = OMC_CORE(1, 1);
        bad += check("AddGrant Chip RAM, named core", OMC_AddGrant(&job, chip, 64, OMCG_READ), OMCERR_CHIPRAM);
        bad += check("AddGrant Chip RAM left the job", job.omj_NGrants, 0);
        FreeMem(chip, 64);
    }
    OMC_FreeGrant(g1); OMC_FreeGrant(g2); OMC_FreeGrant(g3);
    OMC_FreeGrant(NULL);
    {
        APTR v = AllocVec(64, MEMF_PUBLIC | MEMF_CLEAR);   /* not a grant's: left alone */
        OMC_FreeGrant(v);
        FreeVec(v);
    }
    Printf((STRPTR)"AddGrant: refusals %s\n", (LONG)(bad ? "wrong" : "right"));

    /* real jobs in OMC_AllocGrant memory, on CPU0, any core, and a board core */
    {
        ULONG *d = OMC_AllocGrant(size * 4, 0), *o = OMC_AllocGrant(16, OMCAF_CLEAR);
        ULONG targets[3] = { OMC_CPU0, OMC_ANY, OMC_ANY }, nt = units ? 3 : 2;
        if (!d || !o) { PutStr((STRPTR)"  FAIL AllocGrant: no memory\n"); return bad + 1; }
        for (ULONG i = 0; i < size; i++) d[i] = i * 7 + 3;
        for (ULONG t = 0; t < nt; t++) {
            o[0] = o[1] = 0;
            bad += check("AddGrant for a job", setup2(&job, a3, d, size, o, targets[t]), OMCERR_OK);
            if (t == 2) job.omj_Flags |= OMCF_BOARD;
            OMC_Run68k(&job);
            Printf((STRPTR)"AllocGrant job, %s: status %ld on %s, sum %s\n",
                   (LONG)(t == 0 ? "CPU0" : t == 1 ? "any" : "board only"), job.omj_Status, (LONG)where(job.omj_Where),
                   (LONG)(o[0] == want && (LONG)job.omj_Regs[0] == want_d0 ? "right" : "wrong"));
            if (job.omj_Status || o[0] != want || (LONG)job.omj_Regs[0] != want_d0) bad++;
        }

        /* a completion signal over SIGJOBS jobs at once: any core, then CPU0 alone */
        {
            BYTE sig = AllocSignal(-1);
            ULONG mask = 1UL << sig;
            if (sig < 0) { PutStr((STRPTR)"  FAIL no signal bit\n"); return bad + 1; }
            for (ULONG pass = 0; pass < 2; pass++) {
                ULONG done = 0, right = 0, wakes = 0, onboard = 0, free0, free1, left = SIGJOBS;
                UBYTE collected[SIGJOBS];
                for (ULONG k = 0; k < SIGJOBS; k++) {
                    collected[k] = 0;
                    if (!(outs[k] = OMC_AllocGrant(16, OMCAF_CLEAR))) { PutStr((STRPTR)"  FAIL AllocGrant\n"); return bad + 1; }
                }
                SetSignal(0, mask);
                free0 = free_signals();
                for (ULONG k = 0; k < SIGJOBS; k++) {
                    setup2(&jobs[k], jargs[k], d, size, outs[k], pass ? OMC_CPU0 : OMC_ANY);
                    bad += check("SetSignal", OMC_SetSignal(&jobs[k], NULL, mask), OMCERR_OK);
                    if (OMC_Submit(&jobs[k]) != OMCERR_OK) bad++;
                }
                free1 = free_signals();
                while (left && wakes < 10000) {
                    ULONG got = Wait(mask | SIGBREAKF_CTRL_C);
                    wakes++;
                    if (got & SIGBREAKF_CTRL_C) break;
                    for (ULONG k = 0; k < SIGJOBS; k++)
                        if (!collected[k] && OMC_Check(&jobs[k])) {
                            collected[k] = 1; left--; done++;
                            if (OMC_Wait(&jobs[k]) == OMCERR_OK && outs[k][0] == want) right++;
                            if (jobs[k].omj_Where == OMCW_BOARD) onboard++;
                        }
                }
                Printf((STRPTR)"signal, %s: %ld jobs at once, %ld collected, %ld right, %ld on a board, %ld wake-ups, free signals %ld then %ld\n",
                       (LONG)(pass ? "CPU0 alone" : "any core"), (LONG)SIGJOBS, done, right, onboard, wakes, free0, free1);
                if (right != SIGJOBS) bad++;
                if (free1 != free0) { PutStr((STRPTR)"  FAIL jobs held signal bits\n"); bad++; }
                if (!pass && units && !onboard) { PutStr((STRPTR)"  FAIL no job reached the board\n"); bad++; }
                for (ULONG k = 0; k < SIGJOBS; k++) OMC_FreeGrant(outs[k]);
            }
            /* jobs still running when the task sleeps: uneven spins, woken by each finish */
            {
                static ULONG spins[SIGJOBS];
                LONG expect[8];
                ULONG done = 0, right = 0, wakes = 0, left = SIGJOBS;
                UBYTE collected[SIGJOBS];
                for (ULONG v = 0; v < 8; v++) expect[v] = spin_job(1000000UL * (v + 1));   /* the answers, as plain calls */
                SetSignal(0, mask);
                for (ULONG k = 0; k < SIGJOBS; k++) {
                    collected[k] = 0;
                    spins[k] = 1000000UL * (k % 8 + 1);
                    OMC_JobInit(&jobs[k]);
                    jobs[k].omj_Entry = (APTR)spin_job; jobs[k].omj_Args = &spins[k]; jobs[k].omj_NArgs = 1;
                    OMC_SetSignal(&jobs[k], NULL, mask);
                    if (OMC_Submit(&jobs[k]) != OMCERR_OK) bad++;
                }
                while (left && wakes < 10000) {
                    ULONG got = Wait(mask | SIGBREAKF_CTRL_C);
                    wakes++;
                    if (got & SIGBREAKF_CTRL_C) break;
                    for (ULONG k = 0; k < SIGJOBS; k++)
                        if (!collected[k] && OMC_Check(&jobs[k])) {
                            collected[k] = 1; left--; done++;
                            if (OMC_Wait(&jobs[k]) == OMCERR_OK && (LONG)jobs[k].omj_Regs[0] == expect[k % 8]) right++;
                        }
                }
                Printf((STRPTR)"signal, uneven jobs: %ld collected, %ld right, %ld wake-ups\n", done, right, wakes);
                if (right != SIGJOBS) bad++;
                if (units && wakes < 2) { PutStr((STRPTR)"  FAIL the task never slept on a running job\n"); bad++; }
            }

            /* SetSignal refuses two bits, and a bit the task hasn't allocated */
            OMC_JobInit(&job);
            bad += check("SetSignal two bits", OMC_SetSignal(&job, NULL, mask | (mask << 1 ? mask << 1 : 2)), OMCERR_BADJOB);
            {
                ULONG unalloc = 0;
                for (int b = 31; b >= 16; b--) if (!(FindTask(NULL)->tc_SigAlloc & (1UL << b))) { unalloc = 1UL << b; break; }
                if (unalloc) bad += check("SetSignal unallocated bit", OMC_SetSignal(&job, NULL, unalloc), OMCERR_BADJOB);
            }
            FreeSignal(sig);
        }
        OMC_FreeGrant(d); OMC_FreeGrant(o);
    }

    /* OMC_Abort on a long job on a board: OMCERR_CANCEL (or OK, if it finished first) */
    if (units) {
        ULONG n = 400000000UL;
        OMC_JobInit(&job);
        job.omj_Entry = (APTR)spin_job; job.omj_Args = &n; job.omj_NArgs = 1;
        job.omj_Flags = OMCF_BOARD; job.omj_TimeoutMS = 60000;
        if (OMC_Submit(&job) == OMCERR_OK) {
            LONG st;
            OMC_Abort(&job);
            st = OMC_Wait(&job);
            Printf((STRPTR)"abort: status %ld (%s)\n", st, (LONG)(st == OMCERR_CANCEL ? "cancelled" : st == OMCERR_OK ? "finished first" : "wrong"));
            if (st != OMCERR_CANCEL && st != OMCERR_OK) bad++;
        } else bad++;
    }
    return bad;
}

/* No library: what a port does then, the same function as plain calls. */
static int no_library(ULONG size)
{
    ULONG t = 0, x = 0, o[2], ok = 0;
    for (ULONG i = 0; i < size; i++) { t += data[i]; x ^= data[i] * 2654435761UL; }
    for (ULONG k = 0; k < SIGJOBS; k++) {
        o[0] = o[1] = 0;
        if (sum_job(data, size, o) == (LONG)(t ^ x) && o[0] == t && o[1] == x) ok++;
    }
    Printf((STRPTR)"no library: %ld plain calls, %ld right\n%s\n", (LONG)SIGJOBS, ok, (LONG)(ok == SIGJOBS ? "PASS" : "FAIL"));
    return ok == SIGJOBS ? 0 : 10;
}

int main(void)
{
    LONG a[3] = { 0, 0, 0 };
    struct RDArgs *rd = ReadArgs((STRPTR)"COUNT/K/N,SIZE/K/N,NOLIB/S", a, NULL);
    ULONG count = a[0] ? *(ULONG *)a[0] : 20, size = a[1] ? *(ULONG *)a[1] : 100000, units, want, bad = 0;
    BOOL nolib = a[2] != 0;
    struct OMCJob job;
    struct OMCCoreInfo ci;
    LONG want_d0;
    if (rd) FreeArgs(rd);
    if (!(data = AllocVec(size * 4, MEMF_FAST | MEMF_PUBLIC)) && !(data = AllocVec(size * 4, MEMF_PUBLIC))) return 20;
    for (ULONG i = 0; i < size; i++) data[i] = i * 7 + 3;
    if (nolib || !(OpenMulticoreBase = OpenLibrary((STRPTR)OPENMULTICORE_NAME, 0))) {
        int rc;
        PutStr((STRPTR)(nolib ? "NOLIB: the library is not opened\n" : "no openmulticore.library\n"));
        rc = no_library(size);
        FreeVec(data);
        return rc;
    }
    Printf((STRPTR)"openmulticore.library %ld.%ld\n", (LONG)OpenMulticoreBase->lib_Version, (LONG)OpenMulticoreBase->lib_Revision);
    if (!(out = AllocVec(16, MEMF_FAST | MEMF_PUBLIC)) && !(out = AllocVec(16, MEMF_PUBLIC))) return 20;
    units = OMC_CoreCount();
    Printf((STRPTR)"cores on boards: %ld\n", units);
    for (ULONG n = 0; n <= units; n++)
        if (OMC_CoreInfo(n, &ci))
            Printf((STRPTR)"  core %ld: board %ld core %ld, ISAs %lx, model %lx, engine %ld, state %ld, load %ld, done %ld\n",
                   n, ci.oci_Board, ci.oci_Core, ci.oci_ISAs, ci.oci_Model, ci.oci_Engine, ci.oci_State, ci.oci_Load, ci.oci_JobsDone);

    /* the main CPU's answer, the one the others must give */
    setup(&job, size, OMC_CPU0);
    OMC_Run68k(&job);
    want_d0 = (LONG)job.omj_Regs[0]; want = out[0];
    Printf((STRPTR)"CPU0: status %ld on %s, D0 %08lx, sum %lu\n", job.omj_Status, (LONG)where(job.omj_Where), want_d0, want);

    {
        ULONG targets[10], nt = 0;
        targets[nt++] = OMC_CPU0;
        targets[nt++] = OMC_ANY;
        for (ULONG c = 1; c <= units && c <= 8; c++) targets[nt++] = OMC_CORE(1, c);
        for (ULONG t = 0; t < nt; t++) {
            struct DateStamp d0, d1;
            ULONG ok = 0;
            LONG ms;
            DateStamp(&d0);
            for (ULONG k = 0; k < count; k++) {
                out[0] = out[1] = 0;
                setup(&job, size, targets[t]);
                OMC_Run68k(&job);
                if (job.omj_Status == 0 && (LONG)job.omj_Regs[0] == want_d0 && out[0] == want) ok++;
                else if (k == 0) Printf((STRPTR)"  first failure: status %ld, D0 %08lx, sum %lu\n", job.omj_Status, job.omj_Regs[0], out[0]);
            }
            DateStamp(&d1);
            ms = ((d1.ds_Minute - d0.ds_Minute) * 60 * 50 + (d1.ds_Tick - d0.ds_Tick)) * 20;
            if (ok != count) bad++;
            Printf((STRPTR)"target %s%ld: %ld/%ld right, last on %s, %ld ms for %ld jobs\n",
                   (LONG)(targets[t] == OMC_CPU0 ? "CPU0" : targets[t] == OMC_ANY ? "ANY" : "core "),
                   targets[t] == OMC_CPU0 || targets[t] == OMC_ANY ? 0L : (LONG)OMC_CORE_NUMBER(targets[t]),
                   ok, count, (LONG)where(job.omj_Where), ms, count);
        }
    }

    /* jobs at once: one on each core, submitted together */
    if (units) {
        static struct OMCJob many[8];
        static ULONG margs[8][3];
        ULONG (*outs)[2] = AllocVec(8 * 8, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
        ULONG n = units > 8 ? 8 : units, ok = 0;
        for (ULONG c = 0; c < n; c++) {
            setup(&many[c], size, OMC_CORE(1, c + 1));
            margs[c][0] = (ULONG)data; margs[c][1] = size; margs[c][2] = (ULONG)outs[c];
            many[c].omj_Args = margs[c];
            many[c].omj_Grants[1].og_Addr = outs[c];
            OMC_Submit(&many[c]);
        }
        for (ULONG c = 0; c < n; c++) if (OMC_Wait(&many[c]) == 0 && outs[c][0] == want) ok++;
        if (ok != n) bad++;
        FreeVec(outs);
        Printf((STRPTR)"%ld jobs at once, one per core: %ld right\n", n, ok);

        /* a fault is a status, not a crash */
        setup(&job, 0, OMC_ANY);
        job.omj_Entry = (APTR)bad_job; job.omj_NArgs = 0; job.omj_Flags = OMCF_BOARD;
        OMC_Run68k(&job);
        Printf((STRPTR)"illegal instruction on a core: status %ld, vector %ld\n", job.omj_Status, job.omj_FaultVector);
        if (job.omj_Status != OMCERR_FAULT) bad++;

        /* strict: only the grants, the program's hunks among them */
        {
            struct Process *pr = (struct Process *)FindTask(NULL);
            struct CommandLineInterface *cli = BADDR(pr->pr_CLI);
            setup(&job, size, OMC_ANY);
            job.omj_Flags = OMCF_STRICT | OMCF_BOARD;
            if (cli) OMC_GrantSeg(&job, cli->cli_Module, 0);
            OMC_Run68k(&job);
            Printf((STRPTR)"strict, %ld grants: status %ld, sum %s\n", job.omj_NGrants, job.omj_Status, (LONG)(out[0] == want ? "right" : "wrong"));
            if (job.omj_Status || out[0] != want) bad++;
        }
    }
    if (OMC_HAS_JOBCALLS(OpenMulticoreBase)) bad += test_v02(units, size, want, want_d0);
    else PutStr((STRPTR)"0.1: the 0.2 calls are not there\n");

    /* the board's counters after the work */
    for (ULONG n = 1; n <= units; n++)
        if (OMC_CoreInfo(n, &ci))
            Printf((STRPTR)"  after: core %ld state %ld, load %ld, done %ld, x86 or ARM64 CPU %ld\n", n, ci.oci_State, ci.oci_Load, ci.oci_JobsDone, (LONG)ci.oci_HostCPU);
    Printf((STRPTR)"%s\n", (LONG)(bad ? "FAIL" : "PASS"));
    FreeVec(data);
    CloseLibrary(OpenMulticoreBase);
    return bad ? 10 : 0;
}
