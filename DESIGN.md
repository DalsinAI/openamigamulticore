<!-- OpenMulticore's design and specification, written in AmigaChrome's development tree on 4 October 2026. Copyright (c) 2026 Dalsin Limited, MIT licence (LICENSE). -->

# OpenMulticore

An open specification for presenting extra processor cores to AmigaOS, and
the library programs use to run work on them. Anyone may build to it: our
runtime, a PiStorm's spare cores, an FPGA board, a PowerPC or ARM card.

We, 4 October 2026: "we should add OpenMulticore for the autoconfig spec
and library"; the product is OpenMulticore and its repository
`DalsinAI/openamigamulticore` (Our naming: products drop "Amiga", repos
carry it). Like every project we call Open, it is MIT, Copyright (c) 2026
Dalsin Limited, with the credit kept.

It builds on the ACMP note of 28 September 2026 (capsule *A1200 Shared Memory
and ACMP*), which settled how a second CPU joins an A1200 without a second
Exec. Its rules stand; OpenMulticore fixes what that note left open (the
register offsets, the record layouts, the library) and opens it to other
hardware.

## 0. Cores by instruction set (5 October 2026)

This section is the rule the rest of the document follows; where an older
section disagrees, this one wins. We, 5 October 2026: "the cores should
present themselves in two ways and advertise themselves as x86 / arm64 /
m68k. Native functions can be built on non-m68k cores, so any application
loaded on the Amiga must identify what kind of CPU it's for, and select a
core. Also any services that will live on those cores must declare what
they need. If no CPU is available for the kind of application, we fall back
to an m68k JIT-style core", and "any cpu core can and must have an m68k jit
option, but native execution is possible too".

- **Every core runs 68k code, always, through a JIT** (a translated core,
  not an interpreter, which stays only as a debugging switch). A core may
  also run native code for its own instruction set: x86-64 on AmigaChrome's
  PC, ARM64 on the Pi appliance and on a PiStorm's spare cores.
- **Cores sit on AutoConfig boards that carry the ACSV block**, the one
  protocol AmigaChrome's services card uses (Dalsin $DA15, MAGIC `'ACSV'`),
  with CLASS 2 for cores. Dalsin's cores board is product 7. Two registers
  follow CLASS: UNITS at `$24` (how many cores; every one runs 68k) and ISAS
  at `$28` (bit 0 m68k, always set; bit 1 x86-64; bit 2 ARM64). The board's
  directory lists `cpu.m68k/1`, plus `cpu.x86_64/1` or `cpu.arm64/1` when the
  host allows native jobs; 68k jobs open `cpu.m68k/1` (RUN, RUNX, INFO, CANCEL) and native jobs `cpu.x86_64/1` or `cpu.arm64/1`. The
  board, its rings and the host side are specified in AmigaChrome's
  `CORES_BOARD.md`; any Amiga program finds the board through
  expansion.library without our software.
- **A file says its CPU by its format, with nothing added** (5 October
  2026, following AROS): a hunk file is 68k; an ELF names its CPU in
  `e_machine` (with AROS's `EI_OSABI` 15 for AROS programs), and runs on a
  core of that CPU, or on a 68k core if it is 68k; a PowerPC ELF goes to a
  PowerPC core. That is the base rule.
- **A module is an optional extra**: a 68k hunk program carrying native ELF
  sections for x86-64 and ARM64 in one file (`PLACEMENT.md` section 4), for
  programs that want native parts and a 68k fallback together, the way a fat
  binary works. A plain executable needs none of it.
- **Placement order:** native code on a core of its instruction set; else
  68k code (a hunk file, a 68k ELF, or a module's 68k section) on a board's
  core; else on the main CPU. A real Amiga with no board always has the last.
- **Services declare what they need**: the instruction sets they have code
  for, memory, and features (FPU, the SIMD level such as SSE4.2, AVX2 or
  NEON). The directory returns that manifest with each name, so a board never
  offers what its host can't run.
- **Native code is the host's machine code**, so it is accepted only from
  modules our Kitchen built and signed, until it runs in a separate helper
  process with no files, no network and no other system calls (a seccomp
  sandbox), seeing only the job's buffers.
- **A caller may say which core** (5 October 2026: "we must allow
  openmulticore to be told which to put it to"). Automatic placement stays
  the default; a job may instead name a core, and the library sends it
  there, waiting while that core is busy (section 4, "Choosing a core").
- **A core source is a board.** Each source in `SYS:Expansion` (section 2) is
  a board carrying the ACSV block, whatever runs behind it.
- **`openmulticore.library` lives in this repository**: guest code on top of
  `openservice.device` (`DalsinAI/openamigaservice`), which finds the boards
  and carries the messages, locally and to a paired Cradle over the LAN.

What it changes below: section 3's register map is replaced by the ACSV
board; the core kinds that matter first are 68k, x86-64 and ARM64 (PowerPC
and RISC-V stay possible as further ISAS bits, later); host-native numbered
functions become native sections; and the phases follow the cores board's
build order (section 7).

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

## 2. Where cores come from: SYS:Expansion

We, 4 October 2026: `SYS:Expansion` "is where we put the cores for the host
CPU and or extra cores for PiStorms, native cores on Pis, RISC-V etc,
including m68k emulated cores running on host cores."

Every source of cores is a driver in `SYS:Expansion`, one icon each, and every
driver does the same thing when it starts: finds its cores, describes each one
(its kind, model, features and origin, section 3), and registers them with
`openmulticore.resource`. The library and placement (`PLACEMENT.md`) see one
list of cores and never care where a core came from. Adding a kind of core is
adding a driver; nothing else changes.

Drivers come in two kinds:

- **Board drivers**, for cores behind an autoconfig board (section 3). The
  icon carries `PRODUCT=manufacturer/product`, and `BindDrivers` starts the
  driver when that board is present and unclaimed, the standard Amiga way.
  The driver claims the board (clears `CDF_CONFIGME`) and registers its
  cores. OpenMulticore's own reference board is Dalsin $DA15, product 7 (the
  number ACMP reserved), IO, 64 KB, with the driver `OpenMulticore`
  (`PRODUCT=55829/7`). Another maker's board ships its own driver under its own
  manufacturer and product; the library never reads a board's registers
  except through a driver that claimed it.
- **Provider drivers**, for cores that aren't on the Zorro bus. They have no
  board for `BindDrivers` to match, so their icon carries `OMC_PROVIDER` instead
  of `PRODUCT`, and `C:OMCStart` (`PLACEMENT.md` section 8) runs every such
  driver in `SYS:Expansion` after opening the library. A provider gives the
  library the same per-core operations as the registers (start, ring, read
  status, latch) as calls.

What goes there, as each exists:

| Driver | Cores it registers | Kind | Origin |
| --- | --- | --- | --- |
| `OpenMulticore` (board) | AmigaChrome's host-native cores: host threads running numbered functions | host-native | host |
| `OpenMulticore` (board) | AmigaChrome's extra AC090 68k cores, emulated in host threads | 68k | emulated |
| `OpenMulticore` (board) | AmigaChrome's PowerPC core (the PPC460 work) | PowerPC | emulated |
| A PiStorm provider | The Raspberry Pi's spare cores, running jobs natively | ARM | native |
| A PiStorm provider | Further 68k cores, Emu68 running them on spare Pi cores | 68k | emulated |
| A RISC-V card's driver | Its RISC-V cores | RISC-V | native |
| A PowerPC or ARM card's driver | Its cores, through its own board | PowerPC, ARM | native |
| An FPGA board's driver | Soft cores (a 68k or RISC-V in the fabric) | 68k, RISC-V | FPGA |

The PiStorm drivers are Emu68's to write; the spec and the resource are open
for them.

Risks on a stock OS 3.2.3:

- `BindDrivers` runs partway through `S:Startup-Sequence`, so cores are never
  there for anything that runs earlier, and nothing needed to boot may depend
  on them.
- A setup that removes `BindDrivers` gets no board drivers; `OMCStart` still
  runs the providers, and CPU0 is always there.
- A driver that finds nothing, or fails, exits quietly and leaves its board
  unclaimed.
- AROS's handling of `SYS:Expansion` drivers is still to be checked.

## 3. The autoconfig board

**Superseded on 5 October 2026 by the ACSV board (section 0).** Dalsin's
cores board and any other maker's now present the ACSV block with CLASS 2,
UNITS and ISAS, and their services in its directory (`CORES_BOARD.md` in
AmigaChrome has the registers, rings and records). The register map below
is the 4 October draft, kept as a record of the job and completion records'
first shape; nothing implements it.

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
| `+$00` | KIND | 0 none, 1 68k, 2 PowerPC, 3 ARM, 4 x86, 5 host-native, 6 RISC-V |
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

- `OMC_CoreCount()`, `OMC_CoreInfo(n, &info)`: kind, model, state, the board,
  and since 5 October its load (below).
- `OMC_AllocGrant(size, flags)`, `OMC_FreeGrant(g)`: memory every core may
  use (MEMF_PUBLIC, aligned to the largest cache line), with its physical
  ranges ready for a job.
- `OMC_Submit(core, &job)`: queue a job (`OMC_ANY` lets the library pick an
  idle core of the right kind); returns an id. Pushes the grants, writes the
  record, then rings.
- **Choosing a core** (5 October 2026). `OMC_Submit` and `OMC_Run68k`
  take a target: `OMC_ANY` (the default: the library picks, least busy
  first), `OMC_CPU0` (the main CPU), or `OMC_CORE(board, n)`, one named core.
  A named core runs the job even when another is idle: a job aimed at a busy
  core waits for it, and `OMCF_NOWAIT` makes it fail with `OMCERR_BUSY`
  instead. A named core that can't run the job (no such core, or a native
  section for another instruction set) fails with `OMCERR_NOCORE`; it never
  moves elsewhere silently. On the cores board the target travels in the
  request's bits 24 to 28 (0 any free core, 1 to 8 that core), and
  `OMC_CoreInfo` reads the board's read-only load registers: jobs queued
  (`$2C`), and from `$100`, 16 bytes a core: state (idle, 68k, native), load
  over the last second (0 to 1000), jobs finished, and the host CPU the core
  is pinned to. These offsets are the AC090 thread's proposal, 5 October
  2026, and follow `CORES_BOARD.md` when it settles them.
- `OMC_Check(id)`, `OMC_Wait(id)`, and a signal on completion
  (`OMC_SetSignal`), from the level 2 server.
- `OMC_Run68k(func, args, n)`: the common case in one call, run a 68k
  function on another 68k core and wait (the `RunPPC` of OpenMulticore).
- `OMC_Latch(on)`: what a game, or `Forbid`-heavy code, uses to stop the
  extra cores.
- A batch interface for maths (We, the same day: "it should allow a maths
  library to call OpenGPU"): a job kind for vector, matrix, FFT and filter
  batches, so a maths library can spread batches across cores, or hand them
  to OpenGPU where a GPU exists (Open RTG's design, section 5).

Rules for code on another core: no OS calls, no chipset, only its grants;
what it needs from the OS it asks for through a completion request.

## 5. In AmigaChrome

Since 5 October the order is the cores board's (section 0, and section 7):
68k JIT cores first, then native x86-64 jobs in the sandboxed helper. The
list below is the 4 October plan.

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
  be OpenMulticore cores through a provider driver in `SYS:Expansion`
  (section 2), as native ARM cores or as further emulated 68k cores. That is Emu68's work
  to do; the spec and the library are open for it.
- **Real cards:** a PowerPC or ARM card with shared RAM implements section 3
  in its logic or firmware, under its own manufacturer and product.
- **AROS 68k:** the same library and spec.

## 7. Phases

Revised 5 October 2026 to the cores board's build order (section 0). The
board and the host side are AmigaChrome's; the library is this repository's.

| Phase | Delivers | Done when |
| --- | --- | --- |
| 0 | The spec: this document and `PLACEMENT.md`, with `CORES_BOARD.md` for the board | Published, MIT |
| 1 | The cores board (ACSV CLASS 2, UNITS, ISAS) with `cpu.m68k/1` on translated AC090 cores; `openmulticore.library` on `openservice.device` with `OMC_Run68k`, grants and signals, and its main-CPU fallback | A 68k function runs on a board core while CPU0 keeps Workbench running, and the same call runs it on CPU0 with no board |
| 2 | The module format (a 68k section and native sections in one file) and the library's choice and fallback | One module runs on a board core, and on a plain A1200 on CPU0 |
| 3 | The sandboxed native helper and `cpu.x86_64/1`; Kitchen's signed native sections | A module's x86-64 section runs on AmigaChrome's PC, and its 68k section where no x86-64 core is |
| 4 | `cpu.arm64/1` on the Pi appliance; notes for Emu68 on a PiStorm | The same module's ARM64 section runs on the Pi |
| 5 | AROS; PowerPC and other ISAS bits, and the `powerpc.library` and `ppc.library` faces, if we take them up | As each appears |

## 8. Tests

- The runtime: the board's registers, rings and latch in `board_test.c`;
  host-native jobs against a reference in C.
- The Amiga: a test program per phase on a scratch copy, comparing each core's
  results with CPU0's, and a stress run with the latch toggled.
- Never on our validation instance until a phase passes on the copy.

## 9. Placing applications

How a launched program reaches the core it needs (ELF headers and an
`"OpenMulticore"` note, marked hunk executables, a kernel table, overrides,
fallbacks when a core is missing, and the loader) is in `PLACEMENT.md`.
Specified 4 October 2026 and revised 5 October for cores by instruction
set; not built.
