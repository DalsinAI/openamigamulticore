# OpenMulticore

An open specification for presenting extra processor cores to AmigaOS, and
`openmulticore.library`, the library programs use to run work on them.
Anyone may build to it: AmigaChrome's runtime, a PiStorm's spare cores, an
FPGA board, a PowerPC or ARM card.

- One Exec, on the motherboard CPU. Extra cores run jobs in shared memory,
  never the OS or the chipset.
- Every core runs 68k code through a JIT, and may also run native code for
  its own instruction set (x86-64 on a PC, ARM64 on a Pi). Cores sit on
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
