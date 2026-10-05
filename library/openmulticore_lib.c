/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * openmulticore.library 0.1: the first version (DESIGN.md sections 0 and
 * 4). It runs a 68k function on a core of a cores board (AmigaChrome's, a
 * Dalsin $DA15 board carrying the ACSV block, CLASS 2, product 7) through
 * openservice.device's cpu.m68k/1 service, or on the main CPU when there is
 * no board, the same function either way. A caller may name the core
 * (OMC_CORE), ask for the main CPU (OMC_CPU0) or leave it to the library
 * (OMC_ANY); the user's settings in ENV:OpenMulticore choose for programs
 * that leave it (PLACEMENT.md section 6). Modules and native sections come
 * later.
 *
 * Built bare by library/build.sh (no startup code, no C library).
 */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include "../include/libraries/openmulticore.h"
#include <devices/openservice.h>   /* openamigaservice's include/ */
#include <openservice.h>

#define REG(r, decl) register decl __asm(#r)   /* bebbo gcc: an argument in a register */
#define LIB_VERSION 0
#define LIB_REVISION 1

#define DALSIN        0xDA15
#define CORES_PRODUCT 7
#define REG_UNITS     0x24
#define REG_ISAS      0x28
#define REG_QUEUED    0x2C          /* AC090's proposal, 5 Oct 2026 */
#define REG_CORE(n)   (0x100 + 16 * ((n) - 1))   /* state, load, jobs done, host CPU */

#define OP_RUNX   2
#define OP_INFO   3
#define OP_CANCEL 0xFFFF

#define RUNBLOCK_SIZE 0x170         /* the run block with its 16 grants (CORES_BOARD.md section 1) */
#define DEFAULT_STACK 16384

/* omj_Private's longwords */
#define P_STATE   0                 /* 0 new, 1 on a board, 2 done */
#define P_IO      1
#define P_PORT    2
#define P_BLOCK   3
#define P_STACK   4
#define P_SSIZE   5
#define P_SWAP    6                 /* a StackSwapStruct: 6, 7, 8 */
#define P_FPU     9
#define P_OPEN    10                /* the service is open on P_IO */

struct OMCBase {
    struct Library lib;
    BPTR seglist;
    struct Library *dos;
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

int start(void) { return -1; }      /* run as a program: nothing (stays first in the file) */

static const char lib_name[] = OPENMULTICORE_NAME;
static const char lib_id[] = "openmulticore.library 0.1 (5.10.2026) OpenMulticore, Dalsin Limited\r\n";
static const char service_name[] = "cpu.m68k/1";

static struct Library *lib_init(REG(d0, struct OMCBase *base), REG(a0, BPTR seglist), REG(a6, struct ExecBase *sys));
static struct Library *lib_open(REG(a6, struct OMCBase *base));
static BPTR lib_close(REG(a6, struct OMCBase *base));
static BPTR lib_expunge(REG(a6, struct OMCBase *base));
static ULONG lib_null(void);
static ULONG OMC_CoreCount(REG(a6, struct OMCBase *base));
static BOOL OMC_CoreInfo(REG(d0, ULONG n), REG(a0, struct OMCCoreInfo *info), REG(a6, struct OMCBase *base));
static LONG OMC_Run68k(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base));
static LONG OMC_Submit(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base));
static LONG OMC_Wait(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base));
static BOOL OMC_Check(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base));
static void OMC_Abort(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base));
static ULONG OMC_GrantSeg(REG(a0, struct OMCJob *job), REG(d0, BPTR seglist), REG(d1, ULONG mode), REG(a6, struct OMCBase *base));

static const APTR lib_vectors[] = {
    (APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
    (APTR)OMC_CoreCount, (APTR)OMC_CoreInfo, (APTR)OMC_Run68k, (APTR)OMC_Submit,
    (APTR)OMC_Wait, (APTR)OMC_Check, (APTR)OMC_Abort, (APTR)OMC_GrantSeg, (APTR)-1,
};
static const struct { ULONG size; const APTR *vectors; APTR data; APTR init; } lib_inittable = {
    sizeof(struct OMCBase), lib_vectors, NULL, (APTR)lib_init,
};
const struct Resident lib_romtag = {
    RTC_MATCHWORD, (struct Resident *)&lib_romtag, (APTR)(&lib_romtag + 1),
    RTF_AUTOINIT, LIB_VERSION, NT_LIBRARY, 0, (char *)lib_name, (char *)lib_id, (APTR)&lib_inittable,
};

/* ---- the library ---------------------------------------------------------------- */

static struct Library *lib_init(REG(d0, struct OMCBase *base), REG(a0, BPTR seglist), REG(a6, struct ExecBase *sys))
{
    SysBase = sys;
    base->seglist = seglist;
    base->lib.lib_Revision = LIB_REVISION;
    base->dos = OpenLibrary("dos.library", 37);
    DOSBase = (struct DosLibrary *)base->dos;
    return &base->lib;
}
static struct Library *lib_open(REG(a6, struct OMCBase *base))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    return &base->lib;
}
static BPTR lib_close(REG(a6, struct OMCBase *base))
{
    base->lib.lib_OpenCnt--;
    if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
        return lib_expunge(base);
    return 0;
}
static BPTR lib_expunge(REG(a6, struct OMCBase *base))
{
    BPTR seglist;
    if (base->lib.lib_OpenCnt) { base->lib.lib_Flags |= LIBF_DELEXP; return 0; }
    seglist = base->seglist;
    Remove(&base->lib.lib_Node);
    if (base->dos) CloseLibrary(base->dos);
    FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seglist;
}
static ULONG lib_null(void) { return 0; }

/* ---- small helpers -------------------------------------------------------------- */

static void zero(void *p, ULONG n) { UBYTE *b = p; while (n--) *b++ = 0; }
static void put32(UBYTE *p, ULONG v) { *(ULONG *)p = v; }             /* the run block is big-endian, as we are */
static ULONG get32(const UBYTE *p) { return *(const ULONG *)p; }

/* The cores board's registers, or NULL. */
static volatile UBYTE *cores_board(void)
{
    struct Library *ExpansionBase = OpenLibrary("expansion.library", 37);
    struct ConfigDev *cd;
    volatile UBYTE *b = NULL;
    if (!ExpansionBase) return NULL;
    if ((cd = FindConfigDev(NULL, DALSIN, CORES_PRODUCT)) && cd->cd_BoardAddr && *(volatile ULONG *)cd->cd_BoardAddr == OPENSERVICE_MAGIC)
        b = cd->cd_BoardAddr;
    CloseLibrary(ExpansionBase);
    return b;
}

/* ---- the board, through openservice.device ------------------------------------- */

static void board_close(struct OMCJob *job)
{
    struct OSRequest *io = (struct OSRequest *)job->omj_Private[P_IO];
    struct MsgPort *port = (struct MsgPort *)job->omj_Private[P_PORT];
    if (io) {
        if (io->os_Req.io_Device && job->omj_Private[P_OPEN]) {
            io->os_Req.io_Command = OSCMD_CLOSE;           /* the service's handle back */
            DoIO((struct IORequest *)io);
        }
        if (io->os_Req.io_Device) CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
    }
    if (port) DeleteMsgPort(port);
    job->omj_Private[P_IO] = job->omj_Private[P_PORT] = job->omj_Private[P_OPEN] = 0;
}

/* openservice.device opened, and cpu.m68k/1 found, for this job: the handle, or -1. */
static LONG board_open(struct OMCJob *job)
{
    struct MsgPort *port = CreateMsgPort();
    struct OSRequest *io = port ? (struct OSRequest *)CreateIORequest(port, sizeof *io) : NULL;
    job->omj_Private[P_PORT] = (ULONG)port;
    job->omj_Private[P_IO] = (ULONG)io;
    if (!io || OpenDevice((STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)io, 0)) {
        if (io) io->os_Req.io_Device = NULL;
        board_close(job);
        return -1;
    }
    zero(io->os_Buf, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = (APTR)service_name;
    io->os_Buf[0].ob_Length = sizeof service_name - 1;
    io->os_Flags = 0;
    io->os_Req.io_Command = OSCMD_OPEN;
    DoIO((struct IORequest *)io);
    if (io->os_Req.io_Error || io->os_Status) { board_close(job); return -1; }
    io->os_Service = (UWORD)io->os_Result;
    job->omj_Private[P_OPEN] = 1;
    io->os_Req.io_Command = OSCMD_WHERE;
    DoIO((struct IORequest *)io);
    job->omj_Where = io->os_Result == OSWHERE_LAN ? OMCW_LAN : OMCW_BOARD;
    return io->os_Service;
}

static void free_job_memory(struct OMCJob *job)
{
    if (job->omj_Private[P_BLOCK]) FreeVec((APTR)job->omj_Private[P_BLOCK]);
    if (job->omj_Private[P_STACK]) FreeVec((APTR)job->omj_Private[P_STACK]);
    job->omj_Private[P_BLOCK] = job->omj_Private[P_STACK] = 0;
}

/* The job's stack: its own, or one the library allocates in Fast RAM, with the
 * stack arguments at the top (first argument lowest). */
static UBYTE *job_stack(struct OMCJob *job, ULONG *size)
{
    UBYTE *s = job->omj_Stack;
    ULONG n = job->omj_StackSize ? job->omj_StackSize : DEFAULT_STACK;
    n = (n + 3) & ~3UL;
    if (!s) {
        if (!(s = AllocVec(n, MEMF_FAST | MEMF_PUBLIC)) && !(s = AllocVec(n, MEMF_ANY | MEMF_PUBLIC))) return NULL;
        job->omj_Private[P_STACK] = (ULONG)s;
    }
    for (ULONG i = 0; i < job->omj_NArgs; i++)
        ((ULONG *)(s + n))[-(LONG)job->omj_NArgs + (LONG)i] = job->omj_Args[i];
    *size = n;
    return s;
}

/* Sends the job to the board (RUNX). 0, or an OMCERR_. */
static LONG board_send(struct OMCJob *job, ULONG core)
{
    struct OSRequest *io = (struct OSRequest *)job->omj_Private[P_IO];
    UBYTE *rb, *stack;
    ULONG ssize, flags = 3, extra = 0;   /* buf0 the run block and buf1 the stack, both written */
    if (!(rb = AllocVec(RUNBLOCK_SIZE, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR)) && !(rb = AllocVec(RUNBLOCK_SIZE, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR)))
        return OMCERR_NOMEM;
    job->omj_Private[P_BLOCK] = (ULONG)rb;
    if (!(stack = job_stack(job, &ssize))) return OMCERR_NOMEM;
    put32(rb + 0x00, 1);
    put32(rb + 0x04, job->omj_Flags & OMCF_STRICT ? 0 : 2);   /* the board is strict unless bit 1 (permissive) */
    for (int i = 0; i < 15; i++) put32(rb + 0x08 + 4 * i, job->omj_Regs[i]);
    put32(rb + 0x44, job->omj_FPCR);
    put32(rb + 0x48, job->omj_TimeoutMS);
    zero(io->os_Buf, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = rb; io->os_Buf[0].ob_Length = RUNBLOCK_SIZE;
    io->os_Buf[1].ob_Data = stack; io->os_Buf[1].ob_Length = ssize;
    /* written grants go in buf2 and buf3 (the board takes no others), then
     * read grants fill what is left of those and the run block's list */
    {
        ULONG slot = 2, k = 0;
        for (int pass = 0; pass < 2; pass++)
            for (ULONG g = 0; g < job->omj_NGrants && g < OMC_MAXGRANTS; g++) {
                const struct OMCGrant *gr = &job->omj_Grants[g];
                int w = (gr->og_Mode & OMCG_WRITE) != 0;
                if (!gr->og_Length || w != (pass == 0)) continue;
                if (slot < 4) {
                    io->os_Buf[slot].ob_Data = gr->og_Addr; io->os_Buf[slot].ob_Length = gr->og_Length;
                    if (w) flags |= 1UL << slot;
                    slot++;
                } else if (w || k >= 16) {
                    return OMCERR_BADJOB;                 /* more than two written grants, or too many */
                } else {
                    UBYTE *e = rb + 0x70 + 16 * k++;
                    put32(e, (ULONG)gr->og_Addr); put32(e + 4, gr->og_Length); put32(e + 8, (gr->og_Mode & 5) ? gr->og_Mode & 5 : OMCG_READ);
                    put32(rb + 0x4C, k);
                }
            }
    }
    if (core) flags |= (core & 0x1F) << 24;           /* the core, 1 to UNITS (bits 24-28) */
    if (job->omj_Flags & OMCF_NOWAIT) flags |= 1UL << 29;   /* no free core (or the named one taken): -8 rather than waiting */
    extra = job->omj_NArgs;
    io->os_Req.io_Command = OSCMD_CALL;
    io->os_Op = OP_RUNX;
    io->os_Flags = flags;
    io->os_Arg = (ULONG)job->omj_Entry;
    io->os_Extra[0] = extra; io->os_Extra[1] = io->os_Extra[2] = io->os_Extra[3] = 0;
    io->os_Status = 0;
    SendIO((struct IORequest *)io);
    job->omj_Private[P_STATE] = 1;
    return 0;
}

/* The board's answer into the job. */
static LONG board_finish(struct OMCJob *job)
{
    struct OSRequest *io = (struct OSRequest *)job->omj_Private[P_IO];
    const UBYTE *rb = (const UBYTE *)job->omj_Private[P_BLOCK];
    LONG status;
    WaitIO((struct IORequest *)io);
    status = io->os_Req.io_Error ? OMCERR_LOST : io->os_Status;
    if (status == -1 || status == -9) status = OMCERR_NOCORE;   /* no such service; a core past UNITS */
    else if (status == -8) status = OMCERR_BUSY;                /* the named core was taken (no wait) */
    if (rb) {
        for (int i = 0; i < 15; i++) job->omj_Regs[i] = get32(rb + 0x08 + 4 * i);
        job->omj_FPCR = get32(rb + 0x44);
        job->omj_FaultVector = get32(rb + 0x4C);
        job->omj_FaultPC = get32(rb + 0x50);
    }
    if (status == OMCERR_FAULT && !job->omj_FaultVector) { job->omj_FaultVector = io->os_Result; job->omj_FaultPC = io->os_Aux; }
    job->omj_Status = status;
    job->omj_Private[P_STATE] = 2;
    board_close(job);
    free_job_memory(job);
    return status;
}

/* ---- the main CPU ---------------------------------------------------------------- */

/* omc_local_call(job, sss): on the job's stack (exec StackSwap), FPCR set when
 * there is an FPU, D0-D7/A0-A6 loaded, the entry called with RTS back to us,
 * the registers stored, the stack swapped back. The job pointer is found
 * again from the task's tc_SPUpper - 4, where omc_local() put it. */
void omc_local_call(struct OMCJob *job, struct StackSwapStruct *sss);
/* the offsets the code below uses */
_Static_assert(__builtin_offsetof(struct OMCJob, omj_FPCR) == 64, "omj_FPCR");
_Static_assert(__builtin_offsetof(struct OMCJob, omj_Private) + 4 * P_FPU == 344, "P_FPU");
_Static_assert(__builtin_offsetof(struct OMCJob, omj_Private) + 4 * P_SWAP == 332, "P_SWAP");
__asm__(
"   .text\n"
"   .even\n"
"   .globl _omc_local_call\n"
"_omc_local_call:\n"
"   movem.l d2-d7/a2-a6,-(sp)\n"
"   move.l  48(sp),a2\n"              /* the job */
"   move.l  52(sp),a0\n"              /* the StackSwapStruct */
"   move.l  4.w,a6\n"
"   jsr     -732(a6)\n"               /* StackSwap: now on the job's stack */
"   sub.l   a1,a1\n"
"   jsr     -294(a6)\n"               /* FindTask(NULL) */
"   move.l  d0,a0\n"
"   move.l  62(a0),a0\n"              /* tc_SPUpper */
"   move.l  -4(a0),a2\n"              /* the job */
"   tst.l   344(a2)\n"             /* omj_Private[P_FPU]: an FPU to set FPCR on */
"   beq.s   1f\n"
"   .chip   68040\n"
"   fmove.l 64(a2),fpcr\n"            /* omj_FPCR */
"   .chip   68020\n"
"1: pea     2f(pc)\n"                 /* where the job returns to */
"   move.l  (a2),-(sp)\n"             /* the entry: reached by RTS */
"   movem.l 4(a2),d0-d7/a0-a6\n"      /* omj_Regs */
"   rts\n"
"2: movem.l d0-d7/a0-a6,-(sp)\n"      /* the job's registers, kept */
"   move.l  4.w,a6\n"
"   sub.l   a1,a1\n"
"   jsr     -294(a6)\n"
"   move.l  d0,a0\n"
"   move.l  62(a0),a0\n"
"   move.l  -4(a0),a2\n"              /* the job again */
"   lea     4(a2),a1\n"
"   moveq   #14,d1\n"
"3: move.l  (sp)+,(a1)+\n"
"   dbra    d1,3b\n"
"   tst.l   344(a2)\n"
"   beq.s   4f\n"
"   .chip   68040\n"
"   fmove.l fpsr,64(a2)\n"
"   .chip   68020\n"
"4: lea     332(a2),a0\n"          /* omj_Private[P_SWAP]: the StackSwapStruct */
"   jsr     -732(a6)\n"               /* back on the caller's stack */
"   movem.l (sp)+,d2-d7/a2-a6\n"
"   rts\n"
);

static LONG run_local(struct OMCJob *job)
{
    struct StackSwapStruct *sss = (struct StackSwapStruct *)&job->omj_Private[P_SWAP];
    UBYTE *stack;
    ULONG size, nargs = job->omj_NArgs, top;
    UWORD attn = SysBase->AttnFlags;
    /* room above the arguments for the job pointer the call finds again */
    job->omj_StackSize = (job->omj_StackSize ? job->omj_StackSize : DEFAULT_STACK);
    if (!(stack = job_stack(job, &size))) return job->omj_Status = OMCERR_NOMEM;
    /* the arguments go one longword lower, under the job pointer at the top */
    top = (ULONG)stack + size;
    for (ULONG i = 0; i < nargs; i++) ((ULONG *)top)[-1 - (LONG)nargs + (LONG)i] = job->omj_Args[i];
    ((ULONG *)top)[-1] = (ULONG)job;
    sss->stk_Lower = stack;
    sss->stk_Upper = top;
    sss->stk_Pointer = (APTR)(top - 4 - 4 * nargs);
    job->omj_Private[P_FPU] = (attn & (AFF_68881 | AFF_68882 | AFF_FPU40)) ? 1 : 0;
    job->omj_Where = OMCW_CPU0;
    job->omj_FaultVector = job->omj_FaultPC = 0;
    omc_local_call(job, sss);
    if (!job->omj_Private[P_FPU]) job->omj_FPCR = 0;
    job->omj_Status = OMCERR_OK;
    job->omj_Private[P_STATE] = 2;
    free_job_memory(job);
    return OMCERR_OK;
}

/* ---- choosing ------------------------------------------------------------------ */

static ULONG parse_target(const char *s)
{
    ULONG a = 0, b = 0;
    int dot = 0;
    while (*s == ' ' || *s == '\t') s++;
    if ((s[0] | 32) == 'c' && (s[1] | 32) == 'p' && (s[2] | 32) == 'u' && s[3] == '0') return OMC_CPU0;
    if ((s[0] | 32) == 'a' && (s[1] | 32) == 'n' && (s[2] | 32) == 'y') return OMC_ANY;
    for (; *s && *s != '\n'; s++) {
        if (*s == '.') { dot = 1; continue; }
        if (*s < '0' || *s > '9') break;
        if (dot) b = b * 10 + (ULONG)(*s - '0'); else a = a * 10 + (ULONG)(*s - '0');
    }
    if (!dot) { b = a; a = 1; }            /* "3": board 1's third core */
    return a && b && b <= 31 ? OMC_CORE(a, b) : OMC_ANY;
}

/* The user's choice for this program (PLACEMENT.md section 6), for jobs that
 * leave it to the library: ENV:OpenMulticore/Apps/<program>, a line
 * OMC_CORE=..., else ENV:OpenMulticore/Core. */
static ULONG user_target(struct OMCBase *base)
{
    char name[64], path[96], buf[160];
    struct Process *pr = (struct Process *)FindTask(NULL);
    LONG n;
    if (!base->dos) return OMC_ANY;
    name[0] = 0;
    if (pr->pr_Task.tc_Node.ln_Type == NT_PROCESS && pr->pr_CLI) {
        if (GetProgramName((STRPTR)buf, sizeof buf)) {
            const char *f = (const char *)FilePart((STRPTR)buf);
            int i = 0;
            while (f[i] && i < 63) { name[i] = f[i]; i++; }
            name[i] = 0;
        }
    } else if (pr->pr_Task.tc_Node.ln_Name) {
        const char *f = pr->pr_Task.tc_Node.ln_Name;
        int i = 0;
        while (f[i] && i < 63) { name[i] = f[i]; i++; }
        name[i] = 0;
    }
    if (name[0]) {
        const char *pre = "OpenMulticore/Apps/";
        int i = 0, j = 0;
        while (pre[j]) path[i++] = pre[j++];
        for (j = 0; name[j] && i < 95; ) path[i++] = name[j++];
        path[i] = 0;
        if ((n = GetVar((STRPTR)path, (STRPTR)buf, sizeof buf, GVF_GLOBAL_ONLY)) > 0) {
            for (char *p = buf; *p; p++)
                if ((p == buf || p[-1] == '\n') && p[0] == 'O' && p[1] == 'M' && p[2] == 'C' && p[3] == '_' &&
                    p[4] == 'C' && p[5] == 'O' && p[6] == 'R' && p[7] == 'E' && (p[8] == '=' || p[8] == ' '))
                    return parse_target(p + 9);
        }
    }
    if ((n = GetVar((STRPTR)"OpenMulticore/Core", (STRPTR)buf, sizeof buf, GVF_GLOBAL_ONLY)) > 0) return parse_target(buf);
    return OMC_ANY;
}

/* Starts the job where it goes. 0, or an OMCERR_ (the job's status too). */
static LONG start_job(struct OMCJob *job, struct OMCBase *base)
{
    ULONG target = job->omj_Target;
    ULONG core = 0;
    if (job->omj_Private[P_STATE] == 1) return job->omj_Status = OMCERR_BADJOB;   /* already on a board */
    job->omj_Private[P_STATE] = 0;
    job->omj_Status = OMCERR_OK;
    if (target == OMC_ANY && !(job->omj_Flags & (OMCF_NOBOARD | OMCF_BOARD))) target = user_target(base);
    if (target == OMC_CPU0 || (job->omj_Flags & OMCF_NOBOARD)) {
        if (job->omj_Flags & OMCF_BOARD) return job->omj_Status = OMCERR_NOCORE;
        return run_local(job);
    }
    if (target != OMC_ANY) {
        if (OMC_CORE_BOARD(target) != 1 || OMC_CORE_NUMBER(target) < 1 || OMC_CORE_NUMBER(target) > 8)
            return job->omj_Status = OMCERR_NOCORE;   /* one board in this version */
        core = OMC_CORE_NUMBER(target);
        {
            volatile UBYTE *b = cores_board();
            if (!b || core > *(volatile ULONG *)(b + REG_UNITS)) return job->omj_Status = OMCERR_NOCORE;
        }
    }
    if (board_open(job) < 0) {
        if (target != OMC_ANY || (job->omj_Flags & OMCF_BOARD)) return job->omj_Status = OMCERR_NOCORE;
        return run_local(job);                          /* no board: the main CPU, the same function */
    }
    {
        LONG r = board_send(job, core);
        if (r) { board_close(job); free_job_memory(job); job->omj_Private[P_STATE] = 2; return job->omj_Status = r; }
    }
    return 0;
}

/* ---- the calls ------------------------------------------------------------------- */

static ULONG OMC_CoreCount(REG(a6, struct OMCBase *base))
{
    volatile UBYTE *b = cores_board();
    return b ? *(volatile ULONG *)(b + REG_UNITS) : 0;
}

static BOOL OMC_CoreInfo(REG(d0, ULONG n), REG(a0, struct OMCCoreInfo *info), REG(a6, struct OMCBase *base))
{
    volatile UBYTE *b;
    UWORD attn = SysBase->AttnFlags;
    if (!info) return FALSE;
    zero(info, sizeof *info);
    info->oci_HostCPU = ~0UL;
    info->oci_ISAs = 1;
    if (n == 0) {                                        /* the main CPU */
        info->oci_Model = attn & AFF_68060 ? 0x68060 : attn & AFF_68040 ? 0x68040 : attn & AFF_68030 ? 0x68030 :
                          attn & AFF_68020 ? 0x68020 : attn & AFF_68010 ? 0x68010 : 0x68000;
        info->oci_State = 1;
        return TRUE;
    }
    if (!(b = cores_board()) || n > *(volatile ULONG *)(b + REG_UNITS)) return FALSE;
    info->oci_Board = 1; info->oci_Core = n;
    info->oci_ISAs = *(volatile ULONG *)(b + REG_ISAS) | 1;
    info->oci_Model = 0x68040; info->oci_Engine = 2;      /* the board's cores: translated 68040s */
    info->oci_State = *(volatile ULONG *)(b + REG_CORE(n));
    info->oci_Load = *(volatile ULONG *)(b + REG_CORE(n) + 4);
    info->oci_JobsDone = *(volatile ULONG *)(b + REG_CORE(n) + 8);
    info->oci_HostCPU = *(volatile ULONG *)(b + REG_CORE(n) + 12);
    return TRUE;
}

static LONG OMC_Run68k(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    LONG r;
    if (!job || !job->omj_Entry) return OMCERR_BADJOB;
    if ((r = start_job(job, base)) || job->omj_Private[P_STATE] == 2) return job->omj_Status;
    return board_finish(job);
}

static LONG OMC_Submit(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    if (!job || !job->omj_Entry) return OMCERR_BADJOB;
    return start_job(job, base);                         /* the main CPU's jobs are done when this returns */
}

static LONG OMC_Wait(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    if (job && job->omj_Private[P_STATE] == 1) return board_finish(job);
    return job ? job->omj_Status : OMCERR_BADJOB;
}

static BOOL OMC_Check(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    if (!job) return TRUE;
    if (job->omj_Private[P_STATE] != 1) return TRUE;
    return CheckIO((struct IORequest *)job->omj_Private[P_IO]) != NULL;
}

static void OMC_Abort(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    if (job && job->omj_Private[P_STATE] == 1) AbortIO((struct IORequest *)job->omj_Private[P_IO]);
}

/* Grants every hunk of a loaded program, so a strict job can run its code
 * and read its data. The number of grants added. */
static ULONG OMC_GrantSeg(REG(a0, struct OMCJob *job), REG(d0, BPTR seglist), REG(d1, ULONG mode), REG(a6, struct OMCBase *base))
{
    ULONG added = 0;
    for (ULONG *seg = BADDR(seglist); seg && job->omj_NGrants < OMC_MAXGRANTS; seg = BADDR(*seg)) {
        struct OMCGrant *g = &job->omj_Grants[job->omj_NGrants++];
        g->og_Addr = seg + 1;
        g->og_Length = seg[-1] - 8;
        g->og_Mode = mode ? mode : OMCG_READ | OMCG_EXEC;
        added++;
    }
    return added;
}
