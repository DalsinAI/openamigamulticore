/* openmulticore.library 0.1: run 68k functions on extra cores, or on the
 * main CPU when there are none (DESIGN.md sections 0 and 4).
 * MIT, Copyright (c) 2026 Dalsin Limited. */
#ifndef LIBRARIES_OPENMULTICORE_H
#define LIBRARIES_OPENMULTICORE_H

#include <exec/types.h>

#define OPENMULTICORE_NAME    "openmulticore.library"
#define OPENMULTICORE_VERSION 0

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
