# Restart: OpenMulticore

_Written 6 October 2026 at about 23:55 UTC, while all work is paused on @SacredTrees's word (23:28 UTC). Read this first when work resumes; the newest capsule and the live PR list win if they disagree._

## What this repo is

OpenMulticore: an open spec and openmulticore.library for running work on extra cores from AmigaOS 3.x: host threads, a PiStorm's spare cores, RISC-V or ARM many-core chips.

## Where it stands

openmulticore.library 0.1 and the placement spec are merged (#5-#9: named cores, no-wait, per-core counters). The RISC-V 64-core cluster design is Thufir's and has not arrived.

## Merged lately

- #10 (b92dce5, 2026-10-06): Credit who made OpenMulticore: CONTRIBUTORS.md
- #9 (9777bc7, 2026-10-05): OMCTest: per-core counters after the work
- #8 (53e0c05, 2026-10-05): No-wait for any core; the board's order and reservation
- #7 (c4f4577, 2026-10-05): Named cores: no-wait bit 29, board statuses -8 and -9
- #6 (0c70db1, 2026-10-05): Placement: a file's format says its CPU; modules optional
- #5 (935f538, 2026-10-05): openmulticore.library 0.1: run a function on a core, or on the main CPU

## Open pull requests

- None.

## Next step

1. Switch on the cores board from Cradle (AC090 thread owns it).
2. PiStorm spare cores once the PiStorm card runs.

## Waiting on @SacredTrees

- Nothing.

## Who owns it

AC090 thread (cores board); Main Discourse.

## Capsules

Restart capsules for this repo's workstreams, in amigachrome's `capjumps/` shelf:

- [`20261006_AmigaChrome_AC090_JIT_OpenGfx_Restart_Capsule.zip`](https://github.com/DalsinAI/amigachrome/tree/main/capjumps)
- [`20261006_AmigaChrome_PiStorm_Restart_Capsule.zip`](https://github.com/DalsinAI/amigachrome/tree/main/capjumps)

Team rules that still hold: commits as SacredTrees with no co-author lines; third-party code only on "yes with review" (licence checked, commit and sha256 pinned, fetched at build, never committed); deploys with deploy_dev.py only, on a typed line.
