/* Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 * Inline calls for bebbo's m68k-amigaos-gcc, from library/openmulticore_lib.sfd. */
#ifndef _INLINE_OPENMULTICORE_H
#define _INLINE_OPENMULTICORE_H
#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif
#ifndef OPENMULTICORE_BASE_NAME
#define OPENMULTICORE_BASE_NAME OpenMulticoreBase
#endif
#define OMC_CoreCount() \
    LP0(0x1e, ULONG, OMC_CoreCount, , OPENMULTICORE_BASE_NAME)
#define OMC_CoreInfo(core, info) \
    LP2(0x24, BOOL, OMC_CoreInfo, ULONG, core, d0, struct OMCCoreInfo *, info, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_Run68k(job) \
    LP1(0x2a, LONG, OMC_Run68k, struct OMCJob *, job, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_Submit(job) \
    LP1(0x30, LONG, OMC_Submit, struct OMCJob *, job, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_Wait(job) \
    LP1(0x36, LONG, OMC_Wait, struct OMCJob *, job, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_Check(job) \
    LP1(0x3c, BOOL, OMC_Check, struct OMCJob *, job, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_Abort(job) \
    LP1NR(0x42, OMC_Abort, struct OMCJob *, job, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_GrantSeg(job, seglist, mode) \
    LP3(0x48, ULONG, OMC_GrantSeg, struct OMCJob *, job, a0, BPTR, seglist, d0, ULONG, mode, d1, , OPENMULTICORE_BASE_NAME)
/* 0.2 (check OMC_HAS_JOBCALLS first) */
#define OMC_JobInit(job) \
    LP1NR(0x4e, OMC_JobInit, struct OMCJob *, job, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_AddGrant(job, addr, length, mode) \
    LP4(0x54, LONG, OMC_AddGrant, struct OMCJob *, job, a0, APTR, addr, a1, ULONG, length, d0, ULONG, mode, d1, , OPENMULTICORE_BASE_NAME)
#define OMC_AllocGrant(size, flags) \
    LP2(0x5a, APTR, OMC_AllocGrant, ULONG, size, d0, ULONG, flags, d1, , OPENMULTICORE_BASE_NAME)
#define OMC_FreeGrant(mem) \
    LP1NR(0x60, OMC_FreeGrant, APTR, mem, a0, , OPENMULTICORE_BASE_NAME)
#define OMC_SetSignal(job, task, sigmask) \
    LP3(0x66, LONG, OMC_SetSignal, struct OMCJob *, job, a0, struct Task *, task, a1, ULONG, sigmask, d0, , OPENMULTICORE_BASE_NAME)
#endif
