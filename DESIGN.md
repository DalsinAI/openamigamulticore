<!-- OpenMulticore's design and specification, written in AmigaChrome's development tree on 4 October 2026. Copyright (c) 2026 Dalsin Limited, MIT licence (LICENSE). -->

# OpenMulticore

An open specification for presenting extra processor cores to AmigaOS, and
the library programs use to run work on them. Anyone may build to it: our
runtime, a PiStorm's spare cores, an FPGA board, a PowerPC or ARM card.

Dale, 4 October 2026: "we should add OpenMulticore for the autoconfig spec
and library"; the product is OpenMulticore and its repository
`DalsinAI/openamigamulticore` (Dale's naming: products drop "Amiga", repos
carry it). Like every project we call Open, it is MIT, Copyright (c) 2026
Dalsin Limited, with the credit kept.

It builds on the ACMP note of 28 September 2026 (capsule *A1200 Shared Memory
and ACMP*), which settled how a second CPU joins an A1200 without a second
Exec. Its rules stand; OpenMulticore fixes what that note left open (the
register offsets, the record layouts, the library) and opens it to other
hardware.

## 1. The rules it keeps

- **One Exec, on CPU0.** The motherboard CPU (or the trapdoor 68040 that
  replaces it) runs Exec, the devices and the chipset. Extra cores never
  touch `$DFF000`, the CIAs or Gayle, and run no kernel of their own.
- **Same RAM, physical addresses.** Cores share the machine's memory. A job
  names memory by physical address and length (a grant), never by a pointer
  in CPU0's MMU map or a host pointer.
- **One writer per page.** Handing a job over moves ownership of its grant;
  two cores never write the same page at once. Caches don't snoop, so the
  sender pushes the grant's range (`CachePreDMA`), then rings; the receiver
  invalidates (`CachePostDMA`) before it reads.
- **The doorbell is a register, the note is in RAM.** Job and completion
  rings live in Fast RAM; the doorbell, the latch and the interrupt status are
  registers in the board's own IO window (never `$E80000`), cache-inhibited.
- **Completion is a normal Zorro interrupt** on level 2, served with
  `AddIntServer`, shared with other IO cards.
- **The latch stops every extra core** while a game takes the machine.
- **Faces, not kernels.** `powerpc.library` (WarpOS) and `ppc.library`
  (PowerUP), and later `arm.library` and `x86.library`, sit on
  `openmulticore.library` when a core of that kind exists.

## 2. Two ways to present cores

- **An autoconfig board** (section 3): the hardware way. OpenMulticore's
  reference board is Dalsin $DA15, product 7 (the number ACMP reserved), IO,
  64 KB. Another maker's board uses its own manufacturer and product and is
  listed in `ENVARC:OpenMulticore/Boards` (manufacturer, product) so the
  library looks at it; the library never reads an unknown board's registers.
- **A provider** registered with `openmulticore.resource`: for systems whose
  extra cores aren't on the Zorro bus, such as a PiStorm, where Emu68 owns the
  Raspberry Pi's spare cores. The provider gives the library the same
  per-core operations (start, ring, read status, latch) as calls instead of
  registers.

## 3. The autoconfig board

64 KB IO window, longwords, big-endian. One board may present several cores;
more boards (the next slot) add more.

| Offset | Register | |
| --- | --- | --- |
| `$0000` | SIG | `"OMC1"` (read) |
| `$0004` | VERSION | major in the high word, minor in the low |
| `$0008` | CORES | cores on this board, 1 to 16 |
| `$000C` | FEATURES | bit 0 shared RAM; 1 host-native jobs; 2 caches coherent with CPU0 (no push needed); 3 per-core MMU |
| `$0010` | INT_STATUS | a bit per core with completions waiting; reading clears it and drops the level 2 line |
| `$0014` | INT_ENABLE | a bit per core |
| `$0018` | LATCH | write 1: stop every core on this board; 0: let them run; read: state |
| `$0100 + $40 × n` | core n's block | below |

Core block:

| Offset | Register | |
| --- | --- | --- |
| `+$00` | KIND | 0 none, 1 68k, 2 PowerPC, 3 ARM, 4 x86, 5 host-native |
| `+$04` | MODEL | for a 68k the model ($020, $030, $040, $060); a PowerPC its PVR; and so on |
| `+$08` | STATE | 0 off, 1 idle, 2 running, 3 latched, 4 faulted |
| `+$0C` | DOORBELL | write: wake this core (the value is a reason, 1 = new jobs) |
| `+$10` | JOBRING | physical base of its job ring (4 KB aligned), set by CPU0 before the first ring |
| `+$14` | JOBRING_SIZE | entries, a power of two |
| `+$18` | DONERING | physical base of its completion ring |
| `+$1C` | DONERING_SIZE | entries |
| `+$20` | ROOT | physical MMU root prepared for it, or 0 |
| `+$24` | FAULT_ADDR | the last refused access (custom chips, Chip RAM while latched, outside its grants) |
| `+$28` | FAULT_CODE | |

The rings' heads and tails sit 64 bytes apart (the largest cache line in
play), each owned by one side: CPU0 writes the job ring's head, the core its
tail; the core writes the completion ring's head, CPU0 its tail.

A job record is 64 bytes:

| Offset | Field |
| --- | --- |
| `+$00` | id |
| `+$04` | kind: 1 run code at ENTRY in this core's instruction set; 2 a host-native function by number; 3 a call into a face (PowerPC `RunPPC` and the like) |
| `+$08` | ENTRY (physical) or function number |
| `+$0C` | stack: physical base and size, 8 bytes |
| `+$14` | grant list: physical address and count, 8 bytes |
| `+$1C` | arguments: 8 longwords |
| `+$3C` | flags |

A completion record is 32 bytes: id, status (0 done, else an error or
fault), two result longwords, the core's cycles or time spent, and a request
field: a job that needs the OS posts a request instead of a result, CPU0
does it (a WarpOS-style bounce to the 68k) and rings the job on.

## 4. openmulticore.library

What programs open. The faces open it too.

- `OMC_CoreCount()`, `OMC_CoreInfo(n, &info)`: kind, model, state, the board.
- `OMC_AllocGrant(size, flags)`, `OMC_FreeGrant(g)`: memory every core may
  use (MEMF_PUBLIC, aligned to the largest cache line), with its physical
  ranges ready for a job.
- `OMC_Submit(core, &job)`: queue a job (`OMC_ANY` lets the library pick an
  idle core of the right kind); returns an id. Pushes the grants, writes the
  record, then rings.
- `OMC_Check(id)`, `OMC_Wait(id)`, and a signal on completion
  (`OMC_SetSignal`), from the level 2 server.
- `OMC_Run68k(func, args, n)`: the common case in one call, run a 68k
  function on another 68k core and wait (the `RunPPC` of OpenMulticore).
- `OMC_Latch(on)`: what a game, or `Forbid`-heavy code, uses to stop the
  extra cores.
- A batch interface for maths (Dale, the same day: "it should allow a maths
  library to call OpenGPU"): a job kind for vector, matrix, FFT and filter
  batches, so a maths library can spread batches across cores, or hand them
  to OpenGPU where a GPU exists (Open RTG's design, section 5).

Rules for code on another core: no OS calls, no chipset, only its grants;
what it needs from the OS it asks for through a completion request.

## 5. In AmigaChrome

- **The board:** OpenMulticore product 7 in the A1200 runtime's autoconfig
  chain, fitted from the instance's Hardware panel ("Extra cores").
- **Host-native jobs first:** a pool of host threads running numbered
  functions (maths batches, codecs, compression) on the machine's memory.
  This is the quickest real gain, and it needs no second 68k.
- **Extra AC090 cores next:** each in a host thread of its own, on the same
  guest memory. The JIT's translation cache and page state are shared today,
  so this phase makes them safe across threads (or gives each core its own)
  before a second 68k runs.
- **A PowerPC core** (the PPC460 core from the Sam460 work) as a 603/604-style
  CPU1, with `powerpc.library` and `ppc.library` as faces: the WarpUp item on
  the roadmap.

## 6. Elsewhere

- **PiStorm:** Emu68 runs the 68k on one of the Pi's cores; the others could
  be OpenMulticore cores through a provider (section 2). That is Emu68's work
  to do; the spec and the library are open for it.
- **Real cards:** a PowerPC or ARM card with shared RAM implements section 3
  in its logic or firmware, under its own manufacturer and product.
- **AROS 68k:** the same library and spec.

## 7. Phases

| Phase | Delivers | Done when |
| --- | --- | --- |
| 0 | The spec (this document, then `SPEC.md` in its repository): registers, records, the library's calls | Published, MIT |
| 1 | The board in the runtime with host-native cores; `openmulticore.library` with jobs, grants, signals; a test program | A maths batch runs on host cores from OS 3.2.3 and the results match CPU0's |
| 2 | Extra AC090 cores in host threads; `OMC_Run68k` | A 68k function runs on core 1 while CPU0 keeps Workbench running; the latch stops it |
| 3 | A PowerPC core; `powerpc.library` and `ppc.library` faces | A WarpOS program runs |
| 4 | The provider interface; a reference provider; notes for Emu68 | A provider registers and runs jobs |
| 5 | AROS; `arm.library` and `x86.library` faces | As each core kind appears |

## 8. Tests

- The runtime: the board's registers, rings and latch in `board_test.c`;
  host-native jobs against a reference in C.
- The Amiga: a test program per phase on a scratch copy, comparing each core's
  results with CPU0's, and a stress run with the latch toggled.
- Never on Dale's validation instance until a phase passes on the copy.

## 9. Placing applications

How a launched program reaches the core it needs (ELF headers and an
`"OpenMulticore"` note, marked hunk executables, a kernel table, overrides,
fallbacks when a core is missing, and the loader) is in `PLACEMENT.md`.
Specified 4 October 2026, not built.
