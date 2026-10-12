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

What programs open. The faces open it too. The calls as built are in
`include/libraries/openmulticore.h`, `library/openmulticore_lib.sfd` and
`library/openmulticore.doc`; this section follows them (8 October 2026: the
first draft's `OMC_Submit(core, &job)` returning an id, and
`OMC_Run68k(func, args, n)`, became the job-based calls below).

Built (0.1, 5 October 2026):

- `OMC_CoreCount()`, `OMC_CoreInfo(n, &info)`: kind, model, state, the board,
  and since 5 October its load (below).
- `OMC_Submit(&job)`: start a job (`struct OMCJob`: the entry, registers,
  stack arguments, up to 16 grants, the target in `omj_Target`). Returns
  `OMCERR_OK` or why not; the job itself is the handle, not an id. Pushes
  the grants, writes the record, then rings. A job for the main CPU has run
  when it returns.
- `OMC_Check(&job)`, `OMC_Wait(&job)`: done yet, and collect (one
  `OMC_Wait` for each `OMC_Submit`).
- `OMC_Abort(&job)`: ask the board to stop it; `OMC_Wait` then says
  `OMCERR_CANCEL` (0.2; 0.1 said `OMCERR_LOST`).
- `OMC_Run68k(&job)`: the common case in one call, `OMC_Submit` and
  `OMC_Wait` (the `RunPPC` of OpenMulticore).
- `OMC_GrantSeg(&job, seglist, mode)`: grant a loaded program's hunks, for
  strict jobs.
- **Choosing a core** (5 October 2026). `OMC_Submit` and `OMC_Run68k`
  take a target: `OMC_ANY` (the default: the library picks, least busy
  first), `OMC_CPU0` (the main CPU), or `OMC_CORE(board, n)`, one named core.
  A named core runs the job even when another is idle: a job aimed at a busy
  core waits for it, and `OMCF_NOWAIT` makes it fail with `OMCERR_BUSY`
  instead. A named core that can't run the job (no such core, or a native
  section for another instruction set) fails with `OMCERR_NOCORE`; it never
  moves elsewhere silently. On the cores board the target travels in the
  request's bits 24 to 28 (0 any free core, 1 to 31 that core; the library
  took 1 to 8 before 0.3), and
  `OMC_CoreInfo` reads the board's read-only load registers: jobs queued
  (`$2C`), and from `$100`, 16 bytes a core: state (idle, 68k, native), load
  over the last second (0 to 1000), jobs finished, and the host CPU the core
  is pinned to (built on the cores board, 5 October 2026). A free core takes
  the jobs named for it before any-core jobs, and `OMCF_NOWAIT` also works
  for `OMC_ANY`: the job fails with `OMCERR_BUSY` when no core is free, and
  when it is admitted it holds the idle core it was given.
Built (0.2, 8 October 2026), at the end of the table, so 0.1 programs keep
working; the version stays 0, so a program checks `OMC_HAS_JOBCALLS(base)`
(revision 2 or later) before calling them:

- `OMC_JobInit(&job)`: clear the job and write the defaults (`OMC_ANY`,
  16 KB stack, 10 s timeout).
- `OMC_AddGrant(&job, addr, length, mode)`: add a grant, refusing at once
  what a board would refuse at submit: a 17th grant or a third written one
  (`OMCERR_NOSLOT`), a written grant off whole 16-byte cache lines or an
  overlap with a written grant (`OMCERR_BADGRANT`), and Chip RAM for a job
  that must reach a board (`OMCERR_CHIPRAM`; for an `OMC_ANY` job it keeps
  the job on the main CPU).
- `OMC_AllocGrant(size, flags)`, `OMC_FreeGrant(mem)`: memory every core may
  use: `MEMF_PUBLIC`, Fast RAM when there is any, aligned to and rounded up to
  64-byte lines (the largest cache line in play), or to 4 KB pages with
  `OMCAF_PAGE`.
- `OMC_SetSignal(&job, task, sigmask)`: a signal on completion. The task is
  signalled when the job can be collected (from the board's reply, or by
  `OMC_Submit` itself when the job ran on the main CPU or was refused), so a
  main loop `Wait()`s on its windows and its jobs together. Jobs share the
  one signal and hold no signal bit of their own; without it each job on a
  board holds one of the submitting task's until `OMC_Wait`.

Built (0.3, 12 October 2026), at the end of the table; a program checks
`OMC_HAS_RANGE(base)` (revision 3 or later):

- **One connection, kept.** The library opens `openservice.device` and finds
  `cpu.m68k/1` once, with the first job that needs a board, and asks the
  board's INFO then; each job's request is a copy of that one. 0.2 opened
  the device and the service for every job: three more round trips (OPEN,
  WHERE, CLOSE) and an `OpenDevice`. The connection closes when the last
  program closes the library.
- `OMC_SubmitRange(&job, count, slice, &failed)`, `OMC_RunRange(...)`: one
  worker over the indices 0 to count - 1, called as `worker(index, args...)`
  with D0 = index. On a board with RANGE (INFO's aux bit 16) it is one
  request: every free core takes the next index as it comes free, and one
  completion ends it. With `slice`, the job's one written grant is cut into
  slices, index i writing only its own. Elsewhere the main CPU calls the
  worker for each index in turn, the same code path. `failed` receives the
  lowest failing index (count when none); after a failure no index starts,
  the slices of the indices that returned are written, the failed one's
  left as it was.
- Named cores up to 31.
- **Run blocks and stacks on pages of their own.** A board maps a job's
  written memory by 4 KB page, and a page it only partly writes goes
  through the board's byte-exact callbacks: a library stack that happened
  to share a page with the program's code made that code run that way, a
  hundred times slower (a lone job of a million turns took 352 ms instead
  of 2.3 ms in one run). 0.3 allocates them page-aligned and in whole
  pages; a program's own written grants are best made with
  `OMC_AllocGrant(size, OMCAF_PAGE)` for the same reason.

Measured (OMCBench, `tools/omcbench.c`, a lab copy of an OS 3.2.3 instance
on a runtime built from AmigaChrome's branch of 12 October: a 10-core board
on a host with 6 cores and 12 threads, under other work; E-clock times):

| | microseconds (three runs) |
| --- | --- |
| An empty job on the main CPU | 2 to 3 |
| An empty job on a board, one at a time | 173 to 218 (1,191 before the runtime's changes below) |
| The same, 16 in flight | 56 to 80 (124 before) |
| What 0.2's connection per job added on top | 88 to 111 |
| An empty RANGE index | 0.6 to 0.7 (8.9 before) |

64 items of 2 million turns of a loop: 219 to 228 ms on the main CPU; 42 to
72 ms as 64 separate jobs (3.2 to 5.3 times); 35 to 39 ms as one range on all
ten cores (5.7 to 6.4 times), and as one range on 1, 2, 4, 6, 8 and 10 cores
about 1, 1.8, 3.1, 4.2, 5.4 and 5.7 times. The main CPU kept 92 to 100% of
its own speed with every core busy. A lone job of a million turns took
about as long on a board core as on the main CPU (2.0 to 2.7 ms against 1.7
to 2.5). So a lone job is worth a board above about 100,000 turns of a loop
(its round trip is about 0.2 ms), and any number of independent items is
worth a range.

The runtime's side of those numbers (AmigaChrome, `CORES_BOARD.md` section
0c): a core keeps what its jobs read mapped from one job to the next (a job
that may read all Fast RAM no longer maps 512 MB again, nor translates its
code again), runs a range's indices back to back, and a completion wakes
the idle machine at once instead of at its next millisecond step.

Strict jobs (OMCF_STRICT) run their code through the same callbacks
wherever it lies in a 4 KB page they are not wholly granted, and a
program's hunks rarely fill whole pages: with OMCBench STRICT the 64 work
items ran about forty times slower than permissive ones. Granting execute
grants by whole pages on the board would fix it, at the cost of strict
reads being exact only to the page for code; that is the Team's call.

Built (0.4, 12 October 2026), on the Team's decision of that evening:

- **Permissive is the default**, as it was in the library: a job may read
  any Fast RAM and ROM. The board's default changed with it: AmigaChrome's
  cores board takes a version 2 run block, permissive unless strict
  (`CORES_BOARD.md` section 0e). Version 1 keeps its meaning (strict unless
  permissive), so 0.3 and every older caller get what they always got.
- **`OMCF_STRICT`** stays the option for secure applications: the grants
  and the stack only, byte exact.
- **`OMCF_PAGES`**, with `OMCF_STRICT`: page grants. Each execute grant may
  be read and run by its whole 4 KB pages, for speed; writes stay byte
  exact, and read grants stay exact. The library puts execute grants (a
  program's hunks from `OMC_GrantSeg` are read and execute) in the run
  block's list, the grants the board widens, before buf2 and buf3.
- The library sends version 2 when the board's INFO has bit 17, and
  version 1 otherwise. An `OMCF_PAGES` job on such a board is plain
  strict: right, only slower.

Measured on the board's side (`cores_bench modes`, AmigaChrome; the code in
a 64-byte execute grant, as a hunk is granted; one translated core, one job
at a time; the host under heavy load from other work):

| Reads | Register loop, MIPS | Mixed loop, MIPS |
| --- | --- | --- |
| permissive | 1,785 to 2,106 | 2,334 to 2,637 |
| strict | 9.5 to 13.8 | 8.2 to 10.8 |
| strict, page grants | 1,681 to 2,018 | 2,298 to 2,558 |

And in the guest (OMCBench with 0.4, a lab copy of Instance-32 on a runtime
built from that change, ten cores; `STRICT` grants the program's hunks with
`OMC_GrantSeg`, `STRICT PAGES` adds `OMCF_PAGES`; one run each):

| | Permissive | Strict | Strict, page grants |
| --- | --- | --- | --- |
| One job of 1,000,000 turns on one board core (main CPU: 2.4 to 2.7 ms) | 3.7 ms | 597 ms | 3.1 ms |
| 64 items of 2,000,000 turns as separate jobs (main CPU: 285 to 314 ms) | 78 ms | 12,463 ms | 113 ms |
| The same as one range on every core | 79 ms | the board's 10 s timeout | 83 ms |

OMCTest passes with 0.4 on that runtime and on the runtime installed
before it (version 1 only), its new case (strict with page grants) among
the rest.

So a strict job with page grants runs its code as fast as a permissive one,
where strict alone runs it about two hundred times slower.

Designed, not built:

- `OMC_Latch(on)`: what a game, or `Forbid`-heavy code, uses to stop the
  extra cores.
- A batch interface for maths (We, the same day: "it should allow a maths
  library to call OpenGPU"): a job kind for vector, matrix, FFT and filter
  batches, so a maths library can spread batches across cores, or hand them
  to OpenGPU where a GPU exists (Open RTG's design, section 5).

Rules for code on another core: no OS calls, no chipset, only its grants;
what it needs from the OS it asks for through a completion request. How to
port threaded programs to these rules: amigachrome
`docs/development/HowTo-Port-Threads-To-OpenMulticore.md`.

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
