/* Copyright (c) 2026 AmigaChrome Team. OpenMulticore, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * openmulticore.library 0.2 (DESIGN.md sections 0 and 4). It runs a 68k
 * function on a core of a cores board (AmigaChrome's, an AmigaChrome Team $DA15
 * board carrying the ACSV block, CLASS 2, product 7) through
 * openservice.device's cpu.m68k/1 service, or on the main CPU when there is
 * no board, the same function either way. A caller may name the core
 * (OMC_CORE), ask for the main CPU (OMC_CPU0) or leave it to the library
 * (OMC_ANY); the user's settings in ENV:OpenMulticore choose for programs
 * that leave it (PLACEMENT.md section 6). Modules and native sections come
 * later. 0.2 adds, at the end of the table: OMC_JobInit, OMC_AddGrant
 * (the grant rules checked as each is added), OMC_AllocGrant and
 * OMC_FreeGrant (memory right for a grant), and OMC_SetSignal (a signal
 * when a job can be collected, with no signal bit held per job). 0.3 keeps
 * one connection to the service for the library, opened by the first job
 * that needs it, so a job costs one round trip instead of four (OPEN,
 * WHERE, the job, CLOSE); adds OMC_SubmitRange and OMC_RunRange (one
 * function over an index range, on every core at once through the board's
 * RANGE, or index by index on the main CPU); and takes named cores up to
 * 31. 0.4 sends the board's version 2 run block where INFO says the board
 * takes it: permissive unless OMCF_STRICT, and OMCF_PAGES (with
 * OMCF_STRICT) for page grants; other boards get version 1, as before.
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
#include <exec/errors.h>
#include <exec/semaphores.h>
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
#define LIB_REVISION 4

#define AMIGACHROME_MFR        0xDA15
#define CORES_PRODUCT 7
#define REG_UNITS     0x24
#define REG_ISAS      0x28
#define REG_QUEUED    0x2C          /* AC090's proposal, 5 Oct 2026 */
#define REG_CORE(n)   (0x100 + 16 * ((n) - 1))   /* state, load, jobs done, host CPU */

#define OP_RUNX   2
#define OP_INFO   3
#define OP_RANGE  4                 /* CORES_BOARD.md section 1, 12 Oct 2026 */
#define INFO_RANGE (1UL << 16)      /* INFO's aux: the board has RANGE */
#define INFO_RUNBLOCK2 (1UL << 17)  /* INFO's aux: the board takes version 2 run blocks (12 Oct 2026) */
#define OPT1_PERMISSIVE 2           /* version 1 options: strict unless this */
#define OPT2_STRICT 1               /* version 2 options: permissive unless this */
#define OPT2_PAGES  4               /* with OPT2_STRICT: execute grants of the list by whole 4 KB pages */
#define RANGE_MAXSLOTS 32           /* stacks a RANGE request carries: the cores it can use at once */
#define OP_CANCEL 0xFFFF

#define RUNBLOCK_SIZE 0x170         /* the run block with its 16 grants (CORES_BOARD.md section 1) */
#define DEFAULT_STACK 16384

/* omj_Private's longwords */
#define P_STATE   0                 /* 0 new, 1 on a board, 2 done */
#define P_IO      1
#define P_PORT    2
#define P_BLOCK   3
#define P_STACK   4
#define P_SIGMASK 5                 /* OMC_SetSignal's mask, or 0 */
#define P_SWAP    6                 /* a StackSwapStruct: 6, 7, 8 (the main CPU's; a range on a board keeps its out pointer in 6) */
#define P_FAILED  6
#define P_FPU     9
#define P_OPEN    10                /* OPEN_ bits */
#define P_SIGTASK 11                /* OMC_SetSignal's task */

#define OPEN_SIGPORT 2              /* P_PORT is the signal port: the job holds no signal bit of its own */
#define OPEN_RANGE   4              /* a RANGE request: P_FAILED is where its failing index goes */

#define GRANT_MAGIC 0x4F4D4347UL    /* 'OMCG': OMC_AllocGrant's header, just below the memory */

struct OMCBase {
    struct Library lib;
    BPTR seglist;
    struct Library *dos;
    /* 0.3: the connection every job's request is a copy of (conn_get) */
    struct SignalSemaphore sem;
    struct OSRequest *conn;         /* openservice.device open, cpu.m68k/1's handle in os_Service */
    LONG conn_state;                /* 0 not tried yet, 1 open, -1 no service (tried again after the last close) */
    ULONG where;                    /* OMCW_BOARD or OMCW_LAN */
    ULONG units, info;              /* INFO's result and aux (0 when it did not answer) */
    ULONG range_max;                /* ENV:OpenMulticore/RangeCores when the connection was made (0: not set) */
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

int start(void) { return -1; }      /* run as a program: nothing (stays first in the file) */

static const char lib_name[] = OPENMULTICORE_NAME;
static const char lib_id[] = "openmulticore.library 0.4 (12.10.2026) OpenMulticore, AmigaChrome Team\r\n";
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
static void OMC_JobInit(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base));
static LONG OMC_AddGrant(REG(a0, struct OMCJob *job), REG(a1, APTR addr), REG(d0, ULONG length), REG(d1, ULONG mode), REG(a6, struct OMCBase *base));
static APTR OMC_AllocGrant(REG(d0, ULONG size), REG(d1, ULONG flags), REG(a6, struct OMCBase *base));
static void OMC_FreeGrant(REG(a0, APTR mem), REG(a6, struct OMCBase *base));
static LONG OMC_SetSignal(REG(a0, struct OMCJob *job), REG(a1, struct Task *task), REG(d0, ULONG sigmask), REG(a6, struct OMCBase *base));
static LONG OMC_SubmitRange(REG(a0, struct OMCJob *job), REG(d0, ULONG count), REG(d1, ULONG slice), REG(a1, ULONG *failed), REG(a6, struct OMCBase *base));
static LONG OMC_RunRange(REG(a0, struct OMCJob *job), REG(d0, ULONG count), REG(d1, ULONG slice), REG(a1, ULONG *failed), REG(a6, struct OMCBase *base));

static const APTR lib_vectors[] = {
    (APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
    (APTR)OMC_CoreCount, (APTR)OMC_CoreInfo, (APTR)OMC_Run68k, (APTR)OMC_Submit,
    (APTR)OMC_Wait, (APTR)OMC_Check, (APTR)OMC_Abort, (APTR)OMC_GrantSeg,
    /* 0.2: new calls only ever go on the end */
    (APTR)OMC_JobInit, (APTR)OMC_AddGrant, (APTR)OMC_AllocGrant, (APTR)OMC_FreeGrant, (APTR)OMC_SetSignal,
    /* 0.3 */
    (APTR)OMC_SubmitRange, (APTR)OMC_RunRange,
    (APTR)-1,
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
    InitSemaphore(&base->sem);
    return &base->lib;
}
static struct Library *lib_open(REG(a6, struct OMCBase *base))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    return &base->lib;
}
static void conn_close(struct OMCBase *base);
static BPTR lib_close(REG(a6, struct OMCBase *base))
{
    base->lib.lib_OpenCnt--;
    if (base->lib.lib_OpenCnt == 0) conn_close(base);   /* no program has a job any more: the connection goes */
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
    if ((cd = FindConfigDev(NULL, AMIGACHROME_MFR, CORES_PRODUCT)) && cd->cd_BoardAddr && *(volatile ULONG *)cd->cd_BoardAddr == OPENSERVICE_MAGIC)
        b = cd->cd_BoardAddr;
    CloseLibrary(ExpansionBase);
    return b;
}

/* ---- the kept connection (0.3) ------------------------------------------------- */

/* openservice.device opened and cpu.m68k/1 found once, by the first job that
 * needs a board, with INFO asked: 1, or -1 when there is no service (asked
 * again after the last program closes the library). Every job's request is
 * a copy of this one (io_Device, io_Unit, the handle): 0.2 opened the
 * device and the service for each job, three more round trips (OPEN, WHERE,
 * CLOSE) and an OpenDevice per job. */
static LONG conn_get(struct OMCBase *base)
{
    LONG st;
    ObtainSemaphore(&base->sem);
    if (!base->conn_state) {
        struct MsgPort *port = CreateMsgPort();
        struct OSRequest *io = port ? (struct OSRequest *)CreateIORequest(port, sizeof *io) : NULL;
        base->conn_state = -1;
        if (io && !OpenDevice((STRPTR)OPENSERVICE_NAME, 0, (struct IORequest *)io, 0)) {
            zero(io->os_Buf, sizeof io->os_Buf);
            io->os_Buf[0].ob_Data = (APTR)service_name;
            io->os_Buf[0].ob_Length = sizeof service_name - 1;
            io->os_Flags = 0;
            io->os_Req.io_Command = OSCMD_OPEN;
            DoIO((struct IORequest *)io);
            if (!io->os_Req.io_Error && !io->os_Status) {
                io->os_Service = (UWORD)io->os_Result;
                io->os_Req.io_Command = OSCMD_WHERE;
                DoIO((struct IORequest *)io);
                base->where = io->os_Result == OSWHERE_LAN ? OMCW_LAN : OMCW_BOARD;
                zero(io->os_Buf, sizeof io->os_Buf);       /* INFO: units, engine, and whether RANGE is there */
                io->os_Req.io_Command = OSCMD_CALL;
                io->os_Op = OP_INFO; io->os_Flags = 0; io->os_Arg = 0;
                io->os_Extra[0] = io->os_Extra[1] = io->os_Extra[2] = io->os_Extra[3] = 0;
                DoIO((struct IORequest *)io);
                base->units = !io->os_Req.io_Error && !io->os_Status ? io->os_Result : 0;
                base->info = !io->os_Req.io_Error && !io->os_Status ? io->os_Aux : 0;
                base->range_max = 0;
                {
                    char buf[16];
                    if (base->dos && GetVar((STRPTR)"OpenMulticore/RangeCores", (STRPTR)buf, sizeof buf, GVF_GLOBAL_ONLY) > 0)
                        for (char *p = buf; *p >= '0' && *p <= '9'; p++) base->range_max = base->range_max * 10 + (ULONG)(*p - '0');
                }
                io->os_Req.io_Message.mn_ReplyPort = NULL;    /* a template only: jobs copy it, with ports of their own */
                base->conn = io;
                base->conn_state = 1;
                io = NULL;
            } else CloseDevice((struct IORequest *)io);
        }
        if (io) DeleteIORequest((struct IORequest *)io);
        if (port) DeleteMsgPort(port);
    }
    st = base->conn_state;
    ReleaseSemaphore(&base->sem);
    return st;
}

/* The last program has closed the library: the handle back, the device
 * closed. Taken off the base first, so a program opening the library
 * meanwhile makes a connection of its own. */
static void conn_close(struct OMCBase *base)
{
    struct OSRequest *io;
    struct MsgPort *port;
    ObtainSemaphore(&base->sem);
    io = base->conn;
    base->conn = NULL;
    base->conn_state = 0;
    base->units = base->info = 0;
    ReleaseSemaphore(&base->sem);
    if (!io) return;
    if ((port = CreateMsgPort())) {
        io->os_Req.io_Message.mn_ReplyPort = port;
        io->os_Req.io_Command = OSCMD_CLOSE;
        DoIO((struct IORequest *)io);
    }
    CloseDevice((struct IORequest *)io);
    if (port) io->os_Req.io_Message.mn_ReplyPort = NULL;
    DeleteIORequest((struct IORequest *)io);
    if (port) DeleteMsgPort(port);
}

/* ---- the board, through openservice.device ------------------------------------- */

/* A port on the signal OMC_SetSignal gave: no signal bit of its own. */
static struct MsgPort *signal_port(struct OMCJob *job)
{
    struct MsgPort *mp = AllocMem(sizeof *mp, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG mask = job->omj_Private[P_SIGMASK];
    UBYTE bit = 0;
    if (!mp) return NULL;
    while (!(mask & 1)) { mask >>= 1; bit++; }
    mp->mp_Node.ln_Type = NT_MSGPORT;
    mp->mp_Flags = PA_SIGNAL;
    mp->mp_SigBit = bit;
    mp->mp_SigTask = (struct Task *)job->omj_Private[P_SIGTASK];
    mp->mp_MsgList.lh_Head = (struct Node *)&mp->mp_MsgList.lh_Tail;   /* NewList, which is amiga.lib's */
    mp->mp_MsgList.lh_Tail = NULL;
    mp->mp_MsgList.lh_TailPred = (struct Node *)&mp->mp_MsgList.lh_Head;
    mp->mp_MsgList.lh_Type = NT_MESSAGE;
    return mp;
}

/* After the library itself waited on a shared signal, it may have taken a
 * signal another job raised: give it back, so the task's own Wait() still
 * sees it (at worst one wake-up with nothing new). */
static void signal_again(struct OMCJob *job)
{
    ULONG mask = job->omj_Private[P_SIGMASK];
    if (mask) Signal((struct Task *)job->omj_Private[P_SIGTASK], mask);
}

/* The job's copy of the connection's request, and its port. */
static void board_close(struct OMCJob *job)
{
    struct OSRequest *io = (struct OSRequest *)job->omj_Private[P_IO];
    struct MsgPort *port = (struct MsgPort *)job->omj_Private[P_PORT];
    ULONG open = job->omj_Private[P_OPEN];
    if (io) DeleteIORequest((struct IORequest *)io);    /* a copy: never opened, so nothing to close */
    if (port) {
        if (open & OPEN_SIGPORT) FreeMem(port, sizeof *port);
        else DeleteMsgPort(port);
    }
    job->omj_Private[P_IO] = job->omj_Private[P_PORT] = job->omj_Private[P_OPEN] = 0;
}

/* A request for this job, a copy of the kept connection's: the handle, or -1
 * (no service, or no memory: the caller's fallback either way). */
static LONG board_open(struct OMCJob *job, struct OMCBase *base)
{
    struct MsgPort *port;
    struct OSRequest *io;
    if (conn_get(base) < 0) return -1;
    if (job->omj_Private[P_SIGMASK]) port = signal_port(job);   /* the answer comes to a port on the caller's signal */
    else port = CreateMsgPort();
    io = port ? (struct OSRequest *)CreateIORequest(port, sizeof *io) : NULL;
    job->omj_Private[P_PORT] = (ULONG)port;
    job->omj_Private[P_IO] = (ULONG)io;
    job->omj_Private[P_OPEN] = job->omj_Private[P_SIGMASK] ? OPEN_SIGPORT : 0;
    if (!io) { board_close(job); return -1; }
    io->os_Req.io_Device = base->conn->os_Req.io_Device;
    io->os_Req.io_Unit = base->conn->os_Req.io_Unit;
    io->os_Service = base->conn->os_Service;
    job->omj_Where = base->where;
    return io->os_Service;
}

/* Memory right for a grant (OMC_AllocGrant's): Fast RAM when there is any,
 * aligned to and rounded up to 64-byte lines, or with OMCAF_PAGE to 4 KB
 * pages. The run blocks and stacks the library makes are on pages of their
 * own (0.3): a board maps a job's written memory by page, and a stack that
 * shared a page with the program's code made that code run through the
 * board's byte-exact callbacks, a hundred times slower. */
static APTR grant_alloc(ULONG size, ULONG flags)
{
    ULONG align = (flags & OMCAF_PAGE) ? OMC_PAGESIZE : OMC_ALLOCALIGN;
    ULONG req = MEMF_PUBLIC | ((flags & OMCAF_CLEAR) ? MEMF_CLEAR : 0);
    ULONG body, total, p, *h;
    UBYTE *raw;
    if (!size || size > 0x7FF00000UL) return NULL;
    body = (size + align - 1) & ~(align - 1);
    total = body + align + 16;
    if (!(raw = AllocMem(total, req | MEMF_FAST)) && !(raw = AllocMem(total, req))) return NULL;
    p = ((ULONG)raw + 16 + align - 1) & ~(align - 1);
    h = (ULONG *)p - 4;                                  /* the header, in the line below */
    h[0] = (ULONG)raw; h[1] = total; h[2] = body; h[3] = GRANT_MAGIC;
    return (APTR)p;
}
static void grant_free(APTR mem)
{
    ULONG *h = (ULONG *)mem - 4;
    if (!mem || h[3] != GRANT_MAGIC) return;             /* not grant_alloc's: left alone */
    h[3] = 0;
    FreeMem((APTR)h[0], h[1]);
}

static void free_job_memory(struct OMCJob *job)
{
    grant_free((APTR)job->omj_Private[P_BLOCK]);
    grant_free((APTR)job->omj_Private[P_STACK]);
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
        n = (n + OMC_PAGESIZE - 1) & ~(ULONG)(OMC_PAGESIZE - 1);   /* whole pages: the board maps them writable, not through its callbacks */
        if (!(s = grant_alloc(n, OMCAF_PAGE))) return NULL;
        job->omj_Private[P_STACK] = (ULONG)s;
    }
    for (ULONG i = 0; i < job->omj_NArgs; i++)
        ((ULONG *)(s + n))[-(LONG)job->omj_NArgs + (LONG)i] = job->omj_Args[i];
    *size = n;
    return s;
}

/* The run block's version and options (CORES_BOARD.md section 1). A board
 * whose INFO has bit 17 takes version 2: permissive unless strict (bit 0),
 * and page grants (bit 2) an option of strict. Any other board gets version
 * 1, strict unless bit 1 (permissive), as 0.3 sent it; there OMCF_PAGES is
 * plain strict. 1 when the job has page grants: its execute grants then go
 * in the run block's list, the only grants a board widens to whole pages. */
static int put_options(UBYTE *rb, const struct OMCJob *job, const struct OMCBase *base)
{
    int strict = (job->omj_Flags & OMCF_STRICT) != 0, pages = strict && (job->omj_Flags & OMCF_PAGES);
    if (base->info & INFO_RUNBLOCK2) {
        put32(rb + 0x00, 2);
        put32(rb + 0x04, strict ? OPT2_STRICT | (pages ? OPT2_PAGES : 0) : 0);
        return pages;
    }
    put32(rb + 0x00, 1);
    put32(rb + 0x04, strict ? 0 : OPT1_PERMISSIVE);
    return 0;
}

/* Where grant gr goes in pass (0 written, 1 execute with page grants, 2 the
 * rest): 1 when it is this pass's. */
static int grant_pass(const struct OMCGrant *gr, int pass, int pages)
{
    int kind = (gr->og_Mode & OMCG_WRITE) ? 0 : pages && (gr->og_Mode & OMCG_EXEC) ? 1 : 2;
    return gr->og_Length && kind == pass;
}

/* Sends the job to the board (RUNX). 0, or an OMCERR_. */
static LONG board_send(struct OMCJob *job, struct OMCBase *base, ULONG core)
{
    struct OSRequest *io = (struct OSRequest *)job->omj_Private[P_IO];
    UBYTE *rb, *stack;
    ULONG ssize, flags = 3, extra = 0;   /* buf0 the run block and buf1 the stack, both written */
    int pages;
    if (!(rb = grant_alloc(RUNBLOCK_SIZE, OMCAF_PAGE | OMCAF_CLEAR))) return OMCERR_NOMEM;
    job->omj_Private[P_BLOCK] = (ULONG)rb;
    if (!(stack = job_stack(job, &ssize))) return OMCERR_NOMEM;
    pages = put_options(rb, job, base);
    for (int i = 0; i < 15; i++) put32(rb + 0x08 + 4 * i, job->omj_Regs[i]);
    put32(rb + 0x44, job->omj_FPCR);
    put32(rb + 0x48, job->omj_TimeoutMS);
    zero(io->os_Buf, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = rb; io->os_Buf[0].ob_Length = RUNBLOCK_SIZE;
    io->os_Buf[1].ob_Data = stack; io->os_Buf[1].ob_Length = ssize;
    /* written grants go in buf2 and buf3 (the board takes no others); with
     * page grants, execute grants then go in the run block's list while it
     * has room; read grants fill what is left of buf2-buf3 and the list */
    {
        ULONG slot = 2, k = 0;
        for (int pass = 0; pass < 3; pass++)
            for (ULONG g = 0; g < job->omj_NGrants && g < OMC_MAXGRANTS; g++) {
                const struct OMCGrant *gr = &job->omj_Grants[g];
                int w = (gr->og_Mode & OMCG_WRITE) != 0;
                if (!grant_pass(gr, pass, pages)) continue;
                if (slot < 4 && (pass != 1 || k >= 16)) {
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

/* The cores a range may use at once: the board's, capped by
 * ENV:OpenMulticore/RangeCores when that was set as the connection was
 * made (for measuring), at most RANGE_MAXSLOTS. */
static ULONG range_cores(struct OMCBase *base)
{
    ULONG n = base->units ? base->units : 1;
    if (base->range_max && base->range_max < n) n = base->range_max;
    return n > RANGE_MAXSLOTS ? RANGE_MAXSLOTS : n;
}

/* Sends a range to the board (RANGE, op 4): the run block as RUNX's, buf1
 * one stack slot of omj_StackSize per core it may use (the arguments at
 * buf1's top: the board gives every index its own copy after the index),
 * buf2 the written grant cut into count slices, read grants in buf2 (when
 * nothing is written), buf3 and the run block's list. 0, or an OMCERR_. */
static LONG board_send_range(struct OMCJob *job, struct OMCBase *base, ULONG count, ULONG slice)
{
    struct OSRequest *io = (struct OSRequest *)job->omj_Private[P_IO];
    UBYTE *rb, *stack;
    ULONG slot = job->omj_StackSize ? job->omj_StackSize : DEFAULT_STACK, nslot = range_cores(base), n, flags = 3, k = 0, next = slice ? 3 : 2;
    int pages;
    slot = (slot + OMC_PAGESIZE - 1) & ~(ULONG)(OMC_PAGESIZE - 1);   /* whole pages each: a board keeps a core's slot mapped */
    if (!(rb = grant_alloc(RUNBLOCK_SIZE, OMCAF_PAGE | OMCAF_CLEAR))) return OMCERR_NOMEM;
    job->omj_Private[P_BLOCK] = (ULONG)rb;
    n = slot * nslot;
    if (!(stack = grant_alloc(n, OMCAF_PAGE))) return OMCERR_NOMEM;
    job->omj_Private[P_STACK] = (ULONG)stack;
    for (ULONG i = 0; i < job->omj_NArgs; i++)
        ((ULONG *)(stack + n))[-(LONG)job->omj_NArgs + (LONG)i] = job->omj_Args[i];
    pages = put_options(rb, job, base);
    for (int i = 0; i < 15; i++) put32(rb + 0x08 + 4 * i, job->omj_Regs[i]);
    put32(rb + 0x44, job->omj_FPCR);
    put32(rb + 0x48, job->omj_TimeoutMS);
    zero(io->os_Buf, sizeof io->os_Buf);
    io->os_Buf[0].ob_Data = rb; io->os_Buf[0].ob_Length = RUNBLOCK_SIZE;
    io->os_Buf[1].ob_Data = stack; io->os_Buf[1].ob_Length = n;
    for (int pass = 0; pass < 3; pass++)                  /* as board_send: with page grants, execute grants in the list first */
    for (ULONG g = 0; g < job->omj_NGrants && g < OMC_MAXGRANTS; g++) {
        const struct OMCGrant *gr = &job->omj_Grants[g];
        if (!grant_pass(gr, pass, pages)) continue;
        if (gr->og_Mode & OMCG_WRITE) {                   /* the one written grant (start_range checked it) */
            io->os_Buf[2].ob_Data = gr->og_Addr; io->os_Buf[2].ob_Length = gr->og_Length;
            flags |= 4;
        } else if (next < 4 && (pass != 1 || k >= 16)) {
            io->os_Buf[next].ob_Data = gr->og_Addr; io->os_Buf[next].ob_Length = gr->og_Length;
            next++;
        } else if (k >= 16) {
            return OMCERR_BADJOB;                         /* too many */
        } else {
            UBYTE *e = rb + 0x70 + 16 * k++;
            put32(e, (ULONG)gr->og_Addr); put32(e + 4, gr->og_Length); put32(e + 8, (gr->og_Mode & 5) ? gr->og_Mode & 5 : OMCG_READ);
            put32(rb + 0x4C, k);
        }
    }
    io->os_Req.io_Command = OSCMD_CALL;
    io->os_Op = OP_RANGE;
    io->os_Flags = flags;
    io->os_Arg = (ULONG)job->omj_Entry;
    io->os_Extra[0] = job->omj_NArgs; io->os_Extra[1] = count; io->os_Extra[2] = slice; io->os_Extra[3] = slot;
    io->os_Status = 0;
    job->omj_Private[P_OPEN] |= OPEN_RANGE;
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
    if (job->omj_Private[P_OPEN] & OPEN_SIGPORT) {
        struct MsgPort *sp = (struct MsgPort *)job->omj_Private[P_PORT];
        if (!CheckIO((struct IORequest *)io)) {
            if (FindTask(NULL) != sp->mp_SigTask) return OMCERR_BADJOB;   /* only the signalled task can wait on it; the job stays */
            WaitIO((struct IORequest *)io);
            signal_again(job);
        } else WaitIO((struct IORequest *)io);                            /* done: takes the reply off the port */
    } else WaitIO((struct IORequest *)io);
    status = io->os_Req.io_Error == IOERR_ABORTED ? OMCERR_CANCEL       /* OMC_Abort */
           : io->os_Req.io_Error ? OMCERR_LOST : io->os_Status;
    if (status == -1 || status == -9) status = OMCERR_NOCORE;   /* no such service; a core past UNITS */
    else if (status == -8) status = OMCERR_BUSY;                /* the named core was taken (no wait) */
    if (rb) {
        for (int i = 0; i < 15; i++) job->omj_Regs[i] = get32(rb + 0x08 + 4 * i);
        job->omj_FPCR = get32(rb + 0x44);
        job->omj_FaultVector = get32(rb + 0x4C);
        job->omj_FaultPC = get32(rb + 0x50);
    }
    if (job->omj_Private[P_OPEN] & OPEN_RANGE) {       /* the lowest failing index (count: none failed), from aux */
        ULONG *failed = (ULONG *)job->omj_Private[P_FAILED];
        if (failed) *failed = io->os_Req.io_Error ? 0 : io->os_Aux;
        job->omj_Private[P_FAILED] = 0;
    } else if (status == OMCERR_FAULT && !job->omj_FaultVector) { job->omj_FaultVector = io->os_Result; job->omj_FaultPC = io->os_Aux; }
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

/* A range on the main CPU: the worker called for each index in turn, as
 * worker(index, args...) with D0 = index and the job's other registers, on
 * one stack (the job's, or one allocated for all of them). The registers
 * out are the last index's. */
static LONG run_range_local(struct OMCJob *job, ULONG count, ULONG *failed)
{
    ULONG regs[15], a[1 + OMC_RANGEMAXARGS], *args0 = job->omj_Args, n0 = job->omj_NArgs, fpcr = job->omj_FPCR, size0 = job->omj_StackSize;
    APTR stack0 = job->omj_Stack;
    UBYTE *own = NULL;
    for (int r = 0; r < 15; r++) regs[r] = job->omj_Regs[r];
    for (ULONG i = 0; i < n0; i++) a[1 + i] = args0[i];
    if (!stack0) {
        ULONG size = ((size0 ? size0 : DEFAULT_STACK) + 3) & ~3UL;
        if (!(own = grant_alloc(size, OMCAF_PAGE))) return job->omj_Status = OMCERR_NOMEM;
        job->omj_Stack = own; job->omj_StackSize = size;
    }
    job->omj_Args = a; job->omj_NArgs = n0 + 1;
    for (ULONG i = 0; i < count; i++) {
        for (int r = 0; r < 15; r++) job->omj_Regs[r] = regs[r];
        job->omj_Regs[0] = i; job->omj_FPCR = fpcr;
        a[0] = i;
        run_local(job);
    }
    job->omj_Args = args0; job->omj_NArgs = n0; job->omj_Stack = stack0; job->omj_StackSize = size0;
    grant_free(own);
    if (failed) *failed = count;
    job->omj_Where = OMCW_CPU0;
    job->omj_Status = OMCERR_OK;
    job->omj_Private[P_STATE] = 2;
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
        if (OMC_CORE_BOARD(target) != 1 || OMC_CORE_NUMBER(target) < 1 || OMC_CORE_NUMBER(target) > 31)
            return job->omj_Status = OMCERR_NOCORE;   /* one board in this version */
        core = OMC_CORE_NUMBER(target);
        {
            volatile UBYTE *b = cores_board();
            if (!b || core > *(volatile ULONG *)(b + REG_UNITS)) return job->omj_Status = OMCERR_NOCORE;
        }
    }
    if (board_open(job, base) < 0) {
        if (target != OMC_ANY || (job->omj_Flags & OMCF_BOARD)) return job->omj_Status = OMCERR_NOCORE;
        return run_local(job);                          /* no board: the main CPU, the same function */
    }
    {
        LONG r = board_send(job, base, core);
        if (r) { board_close(job); free_job_memory(job); job->omj_Private[P_STATE] = 2; return job->omj_Status = r; }
    }
    return 0;
}

/* Starts a range where it goes: the board's RANGE when it has one, else the
 * main CPU, index by index. 0, or an OMCERR_ (the job's status too). */
static LONG start_range(struct OMCJob *job, struct OMCBase *base, ULONG count, ULONG slice, ULONG *failed)
{
    ULONG target = job->omj_Target, written = 0, wlen = 0;
    if (job->omj_Private[P_STATE] == 1) return job->omj_Status = OMCERR_BADJOB;
    job->omj_Private[P_STATE] = 0;
    job->omj_Status = OMCERR_OK;
    for (ULONG g = 0; g < job->omj_NGrants && g < OMC_MAXGRANTS; g++)
        if ((job->omj_Grants[g].og_Mode & OMCG_WRITE) && job->omj_Grants[g].og_Length) { written++; wlen = job->omj_Grants[g].og_Length; }
    /* the rules a board would apply, checked everywhere, so a program tried on CPU0 is right for a board:
     * one written grant cut into slices, or none; at most OMC_RANGEMAXARGS arguments; any core */
    if (slice ? written != 1 || (count && slice > wlen / count) : written != 0) return job->omj_Status = OMCERR_BADJOB;
    if (job->omj_NArgs > OMC_RANGEMAXARGS || (target != OMC_ANY && target != OMC_CPU0)) return job->omj_Status = OMCERR_BADJOB;
    if (target == OMC_ANY && !(job->omj_Flags & (OMCF_NOBOARD | OMCF_BOARD)) && user_target(base) == OMC_CPU0) target = OMC_CPU0;
    if (target == OMC_CPU0 || (job->omj_Flags & OMCF_NOBOARD)) {
        if (job->omj_Flags & OMCF_BOARD) return job->omj_Status = OMCERR_NOCORE;
        return run_range_local(job, count, failed);
    }
    if (!count) {                                        /* nothing to run */
        if (failed) *failed = 0;
        job->omj_Private[P_STATE] = 2;
        return OMCERR_OK;
    }
    if (conn_get(base) < 0 || !(base->info & INFO_RANGE) || board_open(job, base) < 0) {
        if (job->omj_Flags & OMCF_BOARD) return job->omj_Status = OMCERR_NOCORE;
        return run_range_local(job, count, failed);      /* no board, or one without RANGE: the main CPU, the same worker */
    }
    job->omj_Private[P_FAILED] = (ULONG)failed;
    {
        LONG r = board_send_range(job, base, count, slice);
        if (r) { board_close(job); free_job_memory(job); job->omj_Private[P_FAILED] = 0; job->omj_Private[P_STATE] = 2; return job->omj_Status = r; }
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
    info->oci_Model = 0x68040;                            /* the board's cores: 68040s with their FPU */
    info->oci_Engine = base->info & 0xFF ? base->info & 0xFF : 2;   /* INFO's engine, once the connection asked; else translated */
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
    if (job->omj_Private[P_SIGMASK] && (struct Task *)job->omj_Private[P_SIGTASK] != FindTask(NULL))
        return OMCERR_BADJOB;                            /* it would wait on another task's signal */
    if ((r = start_job(job, base)) || job->omj_Private[P_STATE] == 2) return job->omj_Status;
    return board_finish(job);
}

static LONG OMC_Submit(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    LONG r;
    if (!job || !job->omj_Entry) return OMCERR_BADJOB;
    r = start_job(job, base);                            /* the main CPU's jobs are done when this returns */
    if (job->omj_Private[P_STATE] != 1 && job->omj_Private[P_SIGMASK])
        Signal((struct Task *)job->omj_Private[P_SIGTASK], job->omj_Private[P_SIGMASK]);   /* collectable now: say so as a board would */
    return r;
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

/* ---- 0.2 ------------------------------------------------------------------------- */

/* A fresh job: all zero (omj_Private too), aimed at OMC_ANY, with the
 * default stack and timeout written in. A job on a board is left alone. */
static void OMC_JobInit(REG(a0, struct OMCJob *job), REG(a6, struct OMCBase *base))
{
    if (!job || job->omj_Private[P_STATE] == 1) return;
    zero(job, sizeof *job);
    job->omj_Target = OMC_ANY;
    job->omj_StackSize = OMC_DEFAULTSTACK;
    job->omj_TimeoutMS = OMC_DEFAULTTIMEOUT;
}

static int chip_ram(const UBYTE *a, ULONG n)
{
    return (TypeOfMem((APTR)a) & MEMF_CHIP) || (TypeOfMem((APTR)(a + n - 1)) & MEMF_CHIP);
}

/* Adds a grant, checking the rules the board would only find at submit:
 * slots, written grants, cache lines, overlaps and Chip RAM. OMCERR_OK, or
 * why not (the job is unchanged then). */
static LONG OMC_AddGrant(REG(a0, struct OMCJob *job), REG(a1, APTR addr), REG(d0, ULONG length), REG(d1, ULONG mode), REG(a6, struct OMCBase *base))
{
    const UBYTE *a = addr;
    ULONG n, written = 0;
    struct OMCGrant *g;
    if (!job || job->omj_Private[P_STATE] == 1) return OMCERR_BADJOB;
    if (!a || !length || !(mode & (OMCG_READ | OMCG_WRITE | OMCG_EXEC)) || (mode & ~(ULONG)(OMCG_READ | OMCG_WRITE | OMCG_EXEC)))
        return OMCERR_BADGRANT;
    n = job->omj_NGrants < OMC_MAXGRANTS ? job->omj_NGrants : OMC_MAXGRANTS;
    for (ULONG i = 0; i < n; i++) {
        const struct OMCGrant *o = &job->omj_Grants[i];
        const UBYTE *oa = o->og_Addr;
        if (o->og_Mode & OMCG_WRITE) written++;
        if (a < oa + o->og_Length && oa < a + length && ((mode | o->og_Mode) & OMCG_WRITE))
            return OMCERR_BADGRANT;                      /* one writer: a written range overlaps nothing */
    }
    if (n >= OMC_MAXGRANTS) return OMCERR_NOSLOT;
    if (mode & OMCG_WRITE) {
        if (written >= OMC_MAXWRITTEN) return OMCERR_NOSLOT;
        if (((ULONG)a | length) & (OMC_GRANTALIGN - 1)) return OMCERR_BADGRANT;   /* whole cache lines only */
    }
    if (chip_ram(a, length)) {
        if ((job->omj_Flags & OMCF_BOARD) || (job->omj_Target != OMC_ANY && job->omj_Target != OMC_CPU0))
            return OMCERR_CHIPRAM;                       /* a board can't take Chip RAM */
        job->omj_Flags |= OMCF_NOBOARD;                  /* OMC_ANY: the job stays on the main CPU */
    }
    g = &job->omj_Grants[n];
    g->og_Addr = addr;
    g->og_Length = length;
    g->og_Mode = mode;
    job->omj_NGrants = n + 1;
    return OMCERR_OK;
}

/* Memory right for a grant: Fast RAM when there is any (else other public
 * memory, which a job can still use on the main CPU), starting on a
 * 64-byte line and rounded up to whole lines (OMCAF_PAGE: whole 4 KB
 * pages), so nothing else shares its cache lines. */
static APTR OMC_AllocGrant(REG(d0, ULONG size), REG(d1, ULONG flags), REG(a6, struct OMCBase *base))
{
    return grant_alloc(size, flags);
}

static void OMC_FreeGrant(REG(a0, APTR mem), REG(a6, struct OMCBase *base))
{
    grant_free(mem);                                     /* not OMC_AllocGrant's: left alone */
}

/* From the next OMC_Submit on, sigmask (one signal bit task has allocated)
 * is raised on task (NULL: the caller) when the job can be collected: on a
 * board when it finishes, on the main CPU or on a refusal before
 * OMC_Submit returns. The job then holds no signal bit of its own. 0 turns
 * it off. OMCERR_OK, or OMCERR_BADJOB. */
static LONG OMC_SetSignal(REG(a0, struct OMCJob *job), REG(a1, struct Task *task), REG(d0, ULONG sigmask), REG(a6, struct OMCBase *base))
{
    if (!job || job->omj_Private[P_STATE] == 1) return OMCERR_BADJOB;
    if (!task) task = FindTask(NULL);
    if (sigmask & (sigmask - 1)) return OMCERR_BADJOB;   /* one bit */
    if (sigmask && !(task->tc_SigAlloc & sigmask)) return OMCERR_BADJOB;
    job->omj_Private[P_SIGMASK] = sigmask;
    job->omj_Private[P_SIGTASK] = sigmask ? (ULONG)task : 0;
    return OMCERR_OK;
}

/* ---- 0.3 ------------------------------------------------------------------------- */

/* One worker over the indices 0 to count - 1: as worker(index, args...)
 * with D0 = index, on every core at once through the board's RANGE, or
 * index by index on the main CPU. With slice, the job's one written grant
 * is cut into count slices of slice bytes, and index i writes only slice
 * i. *failed (when given) receives the lowest failing index, or count when
 * none failed, as the range is collected. */
static LONG OMC_SubmitRange(REG(a0, struct OMCJob *job), REG(d0, ULONG count), REG(d1, ULONG slice), REG(a1, ULONG *failed), REG(a6, struct OMCBase *base))
{
    LONG r;
    if (!job || !job->omj_Entry) return OMCERR_BADJOB;
    r = start_range(job, base, count, slice, failed);
    if (job->omj_Private[P_STATE] != 1 && job->omj_Private[P_SIGMASK])
        Signal((struct Task *)job->omj_Private[P_SIGTASK], job->omj_Private[P_SIGMASK]);   /* collectable now */
    return r;
}

static LONG OMC_RunRange(REG(a0, struct OMCJob *job), REG(d0, ULONG count), REG(d1, ULONG slice), REG(a1, ULONG *failed), REG(a6, struct OMCBase *base))
{
    LONG r;
    if (!job || !job->omj_Entry) return OMCERR_BADJOB;
    if (job->omj_Private[P_SIGMASK] && (struct Task *)job->omj_Private[P_SIGTASK] != FindTask(NULL))
        return OMCERR_BADJOB;
    if ((r = start_range(job, base, count, slice, failed)) || job->omj_Private[P_STATE] == 2) return job->omj_Status;
    return board_finish(job);
}
