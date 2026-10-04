# OpenMulticore

An open specification for presenting extra processor cores to AmigaOS, and
`openmulticore.library`, the library programs use to run work on them.
Anyone may build to it: AmigaChrome's runtime, a PiStorm's spare cores, an
FPGA board, a PowerPC or ARM card.

- One Exec, on the motherboard CPU. Extra cores run jobs in shared memory,
  never the OS or the chipset.
- An autoconfig board with a fixed register map (doorbell, latch, job and
  completion rings), or a software provider for cores that aren't on Zorro.
- `openmulticore.library`: cores, memory grants, jobs, completion
  signals, `OMC_Run68k`, and batched maths. `powerpc.library` and
  `ppc.library` (WarpOS and PowerUP) sit on it as faces when a PowerPC core
  exists.

The design and the specification are in `DESIGN.md`. Status, 4 October 2026:
specified; phase 1 (host-native cores in AmigaChrome) is next.

## Licence and credit

OpenMulticore is free software under the MIT licence (`LICENSE`,
Copyright (c) 2026 Dalsin Limited): anyone may use it, change it, fork it and
ship it, commercially too, and hardware makers may implement the
specification freely. The licence's one condition keeps the credit: the
copyright notice and the licence text stay with every copy and fork. We also
ask, as a courtesy rather than a condition, that a fork, a port or an
implementation say it is based on OpenMulticore by Dalsin Limited.

OpenMulticore was created by Dale Kirkwood at Dalsin Limited, for
AmigaChrome.
