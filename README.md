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

## omc_bands(): a loop shared between the cores

`include/openmulticore/bands.h` (11 October 2026), header-only, for a loop
over the rows of a picture:

    static void band(APTR data, ULONG y0, ULONG y1) { ...rows y0 to y1 - 1... }
    omc_bands(NULL, band, &args, rows, out, stride, 0, NULL);

The rows are cut into bands, one for each core and one for the caller,
which runs its own while the cores run theirs. Then it goes through the
bands from the last: a finished band is collected, an unfinished one is
taken back (`OMC_Abort`, `OMC_Wait`) and run by the caller. A cancelled job
never reaches guest RAM (CORES_BOARD.md), so the rows are whole either way,
and a slow or busy board costs the caller about 3 ms (eight bands sent and
taken back) rather than the board's time. After a call that took back every
band, the next `OMC_BANDS_REST` (8) calls skip the board. With no library,
no board, a picture too small to share, or any error on a core, the rows run
on the caller: the same bytes every way. The band function is a job (no OS
calls, writes only its rows); written bands start on 16-byte lines, so the
output buffer must too. `tools/omcbands.c` (OMCBands) checks and times it.

**Why a header, not a library call.** `omc_bands()` is built from the calls
openmulticore.library 0.1 and 0.2 already have (`OMC_Submit`, `OMC_Check`,
`OMC_Abort`, `OMC_Wait`, and `OMC_JobInit`/`OMC_SetSignal` when there), so:

- it works with no library at all: a program built with it opens the
  library itself, and with none installed it calls the band function once
  for every row. An `OMC_Bands()` in the library would need the library
  present just to fall back, and every caller would carry that fallback
  anyway;
- it works with the library already installed (0.1 or 0.2) on every
  instance, with no new library version to deploy first and no new LVO;
- its policy (how many bands, the caller's own band, taking back, resting
  the board) can change per program with a rebuild, while what the board
  needs (grants on 16-byte lines, one written grant per job) stays the
  library's to check;
- the price is one static function in each program that uses it.

If the policy settles and more programs use it, it can move into the
library as `OMC_Bands()` with this header as its fallback, and nothing that
calls it changes.

## BandBench: each kind with 0, 1, 2, 4 and 8 cores

`tools/bandbench.c` (BandBench) times every loop that uses `omc_bands()`
today, each a copy of its program's band function: OpenBench's `cover()`
(1600x1200 24-bit to 1920x1080), `over_gradient()` (1920x1080 with alpha,
in place) and the two in one pass as 0.1.16 does them; openpng.datatype's
colours for RGB and RGBA rows and for Adam7's placing (1920x1080); and a
64x64 call to show the cost of one. Each runs with 0 cores (the plain
loop, what a machine without the library does), through the library aimed
at the main CPU (what it does with no board), and with at most 1, 2, 4 and
8 cores beside the caller, first taking back unfinished bands (what the
programs do) and then waiting for every band (the cores' own speed). Every
way must give the plain loop's bytes, or the kind says DIFFERENT and the
run fails.

    library/build.sh build              (builds BandBench beside OMCTest)
    BandBench                           (every kind, best of 5)
    BandBench REPS 9 KIND 3 QUICK       (one kind, no waiting line)

Run it three ways on the same instance: with openmulticore.library removed
(LIBS:), with the library and the board switched off in Cradle, and with
the board on. Numbers: to be added from Instance-32 (Salvor).

| Kind | 0 | cpu0 | 1 | 2 | 4 | 8 |
| --- | --- | --- | --- | --- | --- | --- |
| cover | | | | | | |
| over | | | | | | |
| cover+over | | | | | | |
| png rgb | | | | | | |
| png rgba | | | | | | |
| adam7 | | | | | | |
| small (64x64) | | | | | | |

## What uses the cores (11 October 2026)

| Program | On the cores |
| --- | --- |
| openmodule.datatype 47.3 | a module's mixing, as a job |
| OpenBench 0.1.16 (openamigabench `tern/backdrop-bands`) | the backdrop's scale and blend (`backdrop_rows`) through `omc_bands`, only when `ENV:OpenMulticore/Bands` exists |
| openpng.datatype 47.4 (openamigaimage `tern/png-bands`) | the colour stage (`png_rows`: rows to ARGB, Adam7's placing) through `omc_bands`, only for pictures of 8 megapixels or more; inflate and the filters stay row-serial |
| OpenMonitor, OpenTitle | open the library to show the cores |
| OMCTest, OMCBands, BandBench, PNGBench | test and time it |

OpenGfx (in opengpu.library) does not: its leaves are host-native code on
the main CPU.

**Measured on Instance-32** with OMCBands and PNGBench by the first
Tern run of 11 October 2026 (AC090 68040, JIT, hyper; cores board, 8
translated cores; ms, best of 5 on a quiet host, best of 3 with the host's
load average at 25 on its 12 threads, in brackets). Every way gave the same bytes in
every run: plain (what a machine without the library does), the library
with no board (the bands run by the library on the main CPU), and the board.

| Work | Plain | Library, no board | 8 cores, waiting for all | 8 cores, `omc_bands` (taking back) |
| --- | --- | --- | --- | --- |
| Backdrop 1600x1200 with alpha, scaled to 1920x1080 and blended | 59.0 (220.8 loaded) | 57.2 | 72.6 | 63.1 (257.5 loaded) |
| The same, 24-bit | 19.4 (25.4) | 19.1 | 13.2 | **11.0** (38.2) |
| 1920x1080 with alpha, blended in place | 46.7 (82.4) | 45.1 | 65.3 | 54.2 (97.3) |
| 1920x1080 24-bit in place (32's boot) | 7.6 (8.8) | 7.8 | 18.6 | 12.2 (22.8) |
| 64x64 (the cost of a call) | 0.0 | 0.1 | 12.3 | 3.1 |
| PNG colours, 1920x1080 RGB (an OpenUp backdrop) | 8.0 (9.0) | 7.4 | 10.9 | 11.3 (20.3) |
| PNG colours, 1920x1080 RGBA (loaded) | 10.3 | 9.7 | 32.9 | 22.3 |
| PNG colours, 1280x1024 RGBA Adam7 (loaded) | 10.6 | 10.3 | 27.0 | 35.9 |

One core runs the blend at about a sixth of the main CPU's speed (half the
rows in 172 ms against all of them in 59 ms); waiting for all, 1, 2, 4 and
8 cores take 172, 122, 76 and 73 ms, so the board stops gaining after four.
The 24-bit scale, with no divisions by 255, is the one loop the board wins:
1.8 times faster on a quiet host, 1.5 times slower on a busy one. A third
run, with the host between the two, had the take-back ahead on both scaled
backdrops (80.0 against 88.7 ms with alpha, 19.7 against 21.2 without) and
behind on everything else. An earlier run the same morning had the board 10
to 50 times slower than the main CPU, so the board's speed depends on the
host's load. Inflate and the filters,
which stay on one core, take 67 to 109 ms of a backdrop PNG's decode, the
colours 8 to 9 ms. DTRead through the installed openpng.datatype 47.3 reads
that backdrop in 9 to 14 ticks and the 1920x1080 RGBA test file in 67;
47.4 does the same work at these sizes.

So today the cores gain only on light loops, only on a quiet host, and not
on anything the desktop does at boot. Where they lose, `omc_bands`' take-back
holds the loss to 3 to 8 ms on a quiet host (13 to 37 ms on a busy one);
waiting for every band lost up to 1.9 s in the earlier run.
OpenBench keeps the bands off unless `ENV:OpenMulticore/Bands` exists (to
try the board again without a new build), and openpng.datatype keeps them
off at screen sizes.

## What next

1. **The board's per-core speed** (AC090 work, amigachrome `cores.c`): a
   translated core runs the blend about six times slower than the main CPU,
   and gains stop after four cores. OMCBands is the measure: when "cores"
   beats "plain" on the blend, turn the bands on.
2. **The cost of a job**: about 1.5 ms each to send and wait, 0.4 ms to send
   and take back. A batch call (several jobs in one request) would help.
3. **PNG**: the part worth sharing is inflate and the filters (about 90% of
   a decode), which are row-serial. A host-native inflate (AC090 magic, as
   for crc32 and adler32) gains more, sooner, than bands.
4. **OpenGfx: do not band its big operations onto the cores.** Its leaves
   are host-native already: at 640x480 an 8-plane fill takes 0.018 ms
   (0.63 ms in 68k code), a 32-bit fill 0.11 ms (0.75), a 32-bit one-line
   scroll 0.033 ms (1.2). Sending eight bands costs 3 to 12 ms before any
   work, 30 to 170 times a whole leaf, and each band would run as 68k code
   at about a sixth of the main CPU's speed. If a large operation needs more than one
   host CPU, the leaf itself should use host threads inside the runtime,
   which needs no change on the Amiga side. The browser's real cost,
   pixman's `over32` (1.05 s a 640x480 frame) and `mask_a8_over32`
   (0.55 s), would gain at most about twice in eight bands at today's core
   speed, where the leaves built so far gained 7 to 36 times: make them
   leaves first, and band only 68k fallbacks, once a core runs near the
   main CPU's speed.

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
