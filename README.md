# OpenMulticore

An open specification for presenting extra processor cores to AmigaOS, and
`openmulticore.library`, the library programs use to run work on them.
Anyone may build to it: AmigaChrome's runtime, a PiStorm's spare cores, an
FPGA board, a PowerPC or ARM card.

- One Exec, on the motherboard CPU. Extra cores run jobs in shared memory,
  never the OS or the chipset.
- Every core runs 68k code through a JIT, and may also run native code for
  its own instruction set (x86-64, or ARM64 on a Pi). Cores sit on
  AutoConfig boards carrying the ACSV block (CLASS 2), which say how many
  cores they have and which instruction sets.
- A program is a module: a 68k section, always there, and optional native
  sections. The library runs the best one present: native on a matching
  core, else 68k on a board core, else 68k on the main CPU, so the same
  file runs on a plain A1200.
- `openmulticore.library` (this repository, on top of `openservice.device`):
  cores, memory grants, jobs, completion signals, `OMC_Run68k`, and batched
  maths. `powerpc.library` and
  `ppc.library` (WarpOS and PowerUP) sit on it as faces when a PowerPC core
  exists.
- Placement: a launched program goes to the core it needs, read from its
  module's sections and an OpenMulticore note, with overrides and fallbacks
  when that core is missing (`PLACEMENT.md`).

The design and the specification are in `DESIGN.md`. Status, 5 October 2026:
specified, revised for cores by instruction set; AmigaChrome is building the
cores board with 68k JIT cores first.

## openmulticore.library 0.1

The first version (5 October 2026) is in `library/`, with its header in
`include/libraries/openmulticore.h`, the SFD, inline and proto headers, and
`tools/omctest.c`. It runs a 68k function (`struct OMCJob`: registers,
stack arguments, grants) on a core of a cores board through
`openservice.device`'s `cpu.m68k/1` service, or on the main CPU when there is
no board, the same function either way:

- `OMC_Run68k(job)`, or `OMC_Submit(job)` then `OMC_Wait`, `OMC_Check`,
  `OMC_Abort`;
- `omj_Target`: `OMC_ANY` (the library chooses), `OMC_CPU0`, or
  `OMC_CORE(board, n)`; for programs that leave it to the library, the
  user's `ENV:OpenMulticore/Apps/<program>` (`OMC_CORE=...`) or
  `ENV:OpenMulticore/Core` chooses;
- `OMC_CoreCount`, `OMC_CoreInfo` (the board's cores and their load);
- `OMC_GrantSeg`, so a strict job may run its program's own code.

## openmulticore.library 0.2

0.2 (8 October 2026) adds five calls at the end of the table; 0.1 programs
keep working, and a program checks `OMC_HAS_JOBCALLS(OpenMulticoreBase)`
before it uses them:

- `OMC_JobInit`: a cleared job with the defaults;
- `OMC_AddGrant`: a grant, with the board's rules checked as it is added;
- `OMC_AllocGrant`, `OMC_FreeGrant`: memory right for a grant;
- `OMC_SetSignal`: a signal when a job can be collected, so a program waits
  on its windows and its jobs together, with no signal bit held per job.

`OMC_Wait` after `OMC_Abort` now says `OMCERR_CANCEL`. The calls are
described in `library/openmulticore.doc`; `library/openmulticore_lib.fd` is
there for other compilers. `tools/omctest.c` tests each of them.

Build: `library/build.sh` with the os32 stove and a checkout of
`openamigaservice` beside this one. Tested on a scratch OS 3.2.3 copy with
AmigaChrome's cores board (`a1200native -C`): the main CPU, any core, a fault
returned as a status, and a strict job all pass. Jobs aimed at a named core
wait for the board to accept the core number (request bits 24 to 28), which
the AC090 work is adding.

## Licence and credit

OpenMulticore is free software under the MIT licence (`LICENSE`,
Copyright (c) 2026 Dalsin Limited): anyone may use it, change it, fork it and
ship it, commercially too, and hardware makers may implement the
specification freely. The licence's one condition keeps the credit: the
copyright notice and the licence text stay with every copy and fork. We also
ask, as a courtesy rather than a condition, that a fork, a port or an
implementation say it is based on OpenMulticore by Dalsin Limited.

OpenMulticore was created by Dalsin Limited, for
AmigaChrome.

## Contributors

OpenMulticore is created and maintained by [SacredTrees](https://github.com/SacredTrees) with the AmigaChrome agent team, copyright Dalsin Limited. Everyone whose work it includes is credited in [`CONTRIBUTORS.md`](CONTRIBUTORS.md).
