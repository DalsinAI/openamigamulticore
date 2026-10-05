/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * OMCTest: openmulticore.library end to end. Lists the cores, then runs one
 * pure 68k function (it sums an array into a written buffer and returns
 * the total) on the main CPU, on any core, and on each named core, checking
 * every answer against the main CPU's and timing them; on a board it also
 * checks a fault comes back as one, and a strict job with its program's
 * hunks granted.
 *   OMCTest [COUNT n] [SIZE n]
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

int main(void)
{
    LONG a[2] = { 0, 0 };
    struct RDArgs *rd = ReadArgs((STRPTR)"COUNT/K/N,SIZE/K/N", a, NULL);
    ULONG count = a[0] ? *(ULONG *)a[0] : 20, size = a[1] ? *(ULONG *)a[1] : 100000, units, want, bad = 0;
    struct OMCJob job;
    struct OMCCoreInfo ci;
    LONG want_d0;
    if (rd) FreeArgs(rd);
    if (!(OpenMulticoreBase = OpenLibrary((STRPTR)OPENMULTICORE_NAME, 0))) { PutStr((STRPTR)"no openmulticore.library\n"); return 20; }
    if (!(data = AllocVec(size * 4, MEMF_FAST | MEMF_PUBLIC)) && !(data = AllocVec(size * 4, MEMF_PUBLIC))) return 20;
    for (ULONG i = 0; i < size; i++) data[i] = i * 7 + 3;
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
    /* the board's counters after the work */
    for (ULONG n = 1; n <= units; n++)
        if (OMC_CoreInfo(n, &ci))
            Printf((STRPTR)"  after: core %ld state %ld, load %ld, done %ld, host CPU %ld\n", n, ci.oci_State, ci.oci_Load, ci.oci_JobsDone, (LONG)ci.oci_HostCPU);
    Printf((STRPTR)"%s\n", (LONG)(bad ? "FAIL" : "PASS"));
    FreeVec(data);
    CloseLibrary(OpenMulticoreBase);
    return bad ? 10 : 0;
}
