/* openmulticore.library 0.2: run 68k functions on extra cores, or on the
 * main CPU when there are none (DESIGN.md sections 0 and 4). 0.2 adds
 * OMC_JobInit, OMC_AddGrant, OMC_AllocGrant, OMC_FreeGrant and
 * OMC_SetSignal (library/openmulticore.doc).
 * MIT, Copyright (c) 2026 Dalsin Limited. */
#ifndef LIBRARIES_OPENMULTICORE_H
#define LIBRARIES_OPENMULTICORE_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/libraries.h>

#define OPENMULTICORE_NAME    "openmulticore.library"
#define OPENMULTICORE_VERSION 0
#define OPENMULTICORE_REVISION_JOBCALLS 2   /* 0.2: the five calls below */

/* The library opened is 0.2 or later: OMC_JobInit, OMC_AddGrant,
 * OMC_AllocGrant, OMC_FreeGrant and OMC_SetSignal are there. The version
 * stays 0, so OpenLibrary can't ask for them: check before calling. */
#define OMC_HAS_JOBCALLS(base) \
    ((base) && ((base)->lib_Version > OPENMULTICORE_VERSION || (base)->lib_Revision >= OPENMULTICORE_REVISION_JOBCALLS))

/* Memory a job may use besides its stack, by address and length. */
struct OMCGrant {
    APTR  og_Addr;
    ULONG og_Length;
    ULONG og_Mode;              /* OMCG_ */
};
#define OMCG_READ  1
#define OMCG_WRITE 2
#define OMCG_EXEC  4

#define OMC_MAXGRANTS 16
#define OMC_MAXWRITTEN 2            /* grants with OMCG_WRITE a job may have */
#define OMC_GRANTALIGN 16           /* a written grant covers whole 68k cache lines: its address and length */
#define OMC_GRANTSIZE(n) (((ULONG)(n) + OMC_GRANTALIGN - 1) & ~(ULONG)(OMC_GRANTALIGN - 1))

/* OMC_AllocGrant's flags */
#define OMCAF_CLEAR (1UL << 0)      /* zeroed */
#define OMCAF_PAGE  (1UL << 1)      /* on its own 4 KB pages, not only its own cache lines */
#define OMC_ALLOCALIGN 64           /* OMC_AllocGrant's alignment and size step (the largest cache line in play) */
#define OMC_PAGESIZE   4096

/* What OMC_JobInit sets */
#define OMC_DEFAULTSTACK   16384
#define OMC_DEFAULTTIMEOUT 10000    /* ms: the board's own default */

/* Where a job runs (omj_Target). */
#define OMC_ANY            0UL                    /* the library chooses (the default) */
#define OMC_CPU0           0xFFFFFFFFUL           /* the main CPU, on the caller's task */
#define OMC_CORE(board, n) (0x10000UL | ((ULONG)(board) << 8) | (ULONG)(n))   /* board 1.., core 1..8 */
#define OMC_CORE_BOARD(t)  (((t) >> 8) & 0xFF)
#define OMC_CORE_NUMBER(t) ((t) & 0xFF)

/* omj_Flags */
#define OMCF_STRICT   (1UL << 0)   /* the job reads and runs only its grants and stack (needed over the LAN) */
#define OMCF_NOWAIT   (1UL << 1)   /* no free core (or the named one is busy): fail with OMCERR_BUSY instead of waiting */
#define OMCF_NOBOARD  (1UL << 2)   /* never use a board: the main CPU only */
#define OMCF_BOARD    (1UL << 3)   /* never the main CPU: fail with OMCERR_NOCORE when no core is there */

/* omj_Status */
#define OMCERR_OK        0
#define OMCERR_BADJOB  (-2)        /* the board refused it: odd entry, stack too small, Chip RAM */
#define OMCERR_CANCEL  (-3)
#define OMCERR_FAULT   (-6)        /* the job took an exception: omj_FaultVector, omj_FaultPC */
#define OMCERR_TIMEOUT (-7)
#define OMCERR_NOCORE  (-20)       /* no core that can run it (a named core that isn't there) */
#define OMCERR_BUSY    (-21)       /* the named core is busy and OMCF_NOWAIT was set */
#define OMCERR_NOMEM   (-22)
#define OMCERR_LOST    (-23)       /* openservice.device lost the board */
#define OMCERR_BADGRANT (-24)      /* OMC_AddGrant: no address or length, no mode, a written grant off a cache line, or overlapping a written one */
#define OMCERR_CHIPRAM  (-25)      /* OMC_AddGrant: Chip RAM for a job that must run on a board */
#define OMCERR_NOSLOT   (-26)      /* OMC_AddGrant: OMC_MAXGRANTS grants, or OMC_MAXWRITTEN written ones, already */

/* omj_Where, after the job */
#define OMCW_CPU0  1
#define OMCW_BOARD 2
#define OMCW_LAN   3

/* A 68k function to run: the registers in and out, its stack, its stack
 * arguments (as GCC's m68k-amigaos convention passes them) and its grants.
 * The rules a job follows are CORES_BOARD.md section 1's: no OS calls, no
 * Chip RAM, its grants and its stack only. */
struct OMCJob {
    APTR   omj_Entry;
    ULONG  omj_Regs[15];        /* D0-D7, A0-A6: in, and out after the job */
    ULONG  omj_FPCR;            /* in; FPSR out (0 on a main CPU without FPU) */
    APTR   omj_Stack;           /* NULL: the library allocates omj_StackSize */
    ULONG  omj_StackSize;       /* 0: 16 KB */
    ULONG  omj_NArgs;           /* longword stack arguments */
    ULONG *omj_Args;
    struct OMCGrant omj_Grants[OMC_MAXGRANTS];
    ULONG  omj_NGrants;
    ULONG  omj_TimeoutMS;       /* 0: the board's default (10 s) */
    ULONG  omj_Flags;           /* OMCF_ */
    ULONG  omj_Target;          /* OMC_ANY, OMC_CPU0 or OMC_CORE() */
    LONG   omj_Status;          /* out: OMCERR_ */
    ULONG  omj_FaultVector, omj_FaultPC;
    ULONG  omj_Where;           /* out: OMCW_ */
    ULONG  omj_Private[12];     /* the library's; 0 before the first use */
};

/* What OMC_CoreInfo says about one core: 0 is the main CPU, 1.. a board's. */
struct OMCCoreInfo {
    ULONG oci_Board;            /* 0 the main CPU, else the board, from 1 */
    ULONG oci_Core;             /* on that board, from 1 */
    ULONG oci_ISAs;             /* bit 0 m68k, bit 1 x86-64, bit 2 ARM64 */
    ULONG oci_Model;            /* $68020, $68040... */
    ULONG oci_Engine;           /* board cores: 1 interpreter, 2 translated (JIT) */
    ULONG oci_State;            /* 0 idle, 1 running 68k, 2 running native (board cores, when the board says) */
    ULONG oci_Load;             /* over the last second, 0 to 1000 (when the board says) */
    ULONG oci_JobsDone;
    ULONG oci_HostCPU;          /* the host CPU the core is pinned to, or ~0 */
};

#endif
