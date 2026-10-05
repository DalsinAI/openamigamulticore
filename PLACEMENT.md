<!-- OpenMulticore placement framework, written 4 October 2026. Copyright (c) 2026 Dalsin Limited, MIT licence (LICENSE). -->

# Placing applications on cores

How a program that is launched (from the Shell, Workbench, `Run` or
`WBStartup`) gets to the core it needs, and what happens when that core isn't
there. It sits on `DESIGN.md`: the registers, records and library calls there
are unchanged except where section 9 says so.

We, 4 October 2026: "the multicore design must have a framework described to
enable launched applications to go where they need to, ELF should let us do
quite a lot here."

Status, 4 October 2026: specified and approved, nothing built. The choices
in section 10 stand at their proposals.

**Revised 5 October 2026 for cores by instruction set** (`DESIGN.md`
section 0, which wins where this document still disagrees). In short:

- Every core runs 68k code through a JIT, and a core may also run native
  code for its own instruction set: x86-64 on a PC, ARM64 on a Pi. Cores sit
  on ACSV boards (CLASS 2); UNITS at `$24` says how many, ISAS at `$28` which
  instruction sets (bit 0 m68k, always; bit 1 x86-64; bit 2 ARM64).
- A program or job is a **module**: a 68k section that is always there, and
  optional native sections, in one file (section 4).
- Placement order: a native section on a core of its instruction set, then
  the 68k section on a board's core, then the main CPU. A real Amiga with no
  board runs the 68k section on its own CPU.
- Services that live on cores declare what they need: instruction sets,
  memory, FPU and SIMD level (section 9).
- Native sections are accepted only from modules our Kitchen built and
  signed, until native jobs run in a sandboxed helper on the host
  (section 10).

## 1. What "placing" means under one Exec

The rule from `DESIGN.md` stands: one Exec, on CPU0. Every launched program
therefore starts as an ordinary Exec process on CPU0, and placing it means
deciding which core runs **its code**, not where its process lives. There are
three shapes, and every program is one of them:

| Shape | Process | Code runs on | Who it's for |
| --- | --- | --- | --- |
| **Anchored** | on CPU0 | CPU0 | Every program today, and every unmarked hunk executable. Nothing changes for it. |
| **Offloading** | on CPU0 | CPU0, plus named kernels on other cores | A 68k program that sends maths, codecs or loops to other cores through `OMC_Submit`. The framework places each kernel. |
| **Hosted** | an anchor on CPU0 | another core, start to finish | A module's native section (x86-64 or ARM64; PowerPC if such cores come), or later its 68k section on a board core, whose OS calls come back to its anchor as completion requests, the WarpOS model. |

The anchor is a small 68k process the loader creates on CPU0. It owns the
program's Exec and DOS identity (its `Process`, its current directory, its
signals, its Workbench message), runs the OS calls the core posts as
requests, and frees everything when the program ends. Shell, `Status`,
`Break` and Workbench see the anchor and treat it like any other process.

## 2. Where placement comes from

Four sources, read in this order. A later one may narrow or override an
earlier one's **preferences**; nothing overrides a **requirement** (an
instruction set, a feature the code can't run without).

1. **The binary itself**: what the code needs, read from the file (sections 3
   and 4). This is the only source of requirements.
2. **The application's settings**: `ENVARC:OpenMulticore/Apps/<name>`, written
   by the user or by an installer, one line per keyword (section 6).
3. **Tooltypes**, when Workbench starts it (section 6).
4. **The command line**: `OMCRun` arguments (section 6).

Defaults fill whatever none of them says: any core of the right kind, the
least busy, fallbacks as the binary lists them.

## 3. ELF executables

Since 5 October an ELF image is how a module carries a **native section**:
`e_machine` `EM_X86_64` (62) or `EM_AARCH64` (183) picks the core, and the
section sits in the module as section 4 describes. A module's 68k section is
its hunk program. What follows is how the loader reads any ELF it is given.

ELF carries most of what placement needs before we add anything, which is why
it's the format of choice for code that runs off CPU0.

**What ELF already says.** The loader reads these without any help from the
program's author:

| Field | Tells the loader |
| --- | --- |
| `e_machine` | The instruction set: `EM_68K` (4), `EM_PPC` (20), `EM_ARM` (40), `EM_AARCH64` (183), `EM_386` (3), `EM_X86_64` (62), `EM_RISCV` (243). This alone picks the core kind. |
| `EI_CLASS`, `EI_DATA` | 32 or 64-bit, and byte order. A little-endian core (ARM, x86) sharing big-endian structures with 68k code is the face's business, not the program's; the loader records the order so the face knows. |
| `e_type` | `ET_DYN` (position-independent): the loader allocates its `PT_LOAD` segments as grants and relocates them. `ET_EXEC` with fixed addresses is refused unless its `PT_LOAD` addresses happen to fall in memory the loader can allocate there. `ET_REL` objects (as PowerUP used) are section-based, with no segments and no entry point, so `OMC_LoadApp` doesn't take them: they load only through the PowerUP face's own `ppc.library` path, as they do today. |
| `e_flags` | Per-architecture ABI bits: ARM EABI version and float ABI, PowerPC's embedded flags. |
| `.ARM.attributes` | `Tag_CPU_arch`, `Tag_FP_arch`, `Tag_Advanced_SIMD_arch`: which ARM, whether VFP, whether NEON. Compilers emit it. |
| `.gnu.attributes` (PowerPC) | `Tag_GNU_Power_ABI_FP` and the vector ABI: hard or soft float, AltiVec. |
| `.note.gnu.property` (x86) | `GNU_PROPERTY_X86_ISA_1_NEEDED`: the x86-64 level (baseline, v2, v3, v4) the code was built for. |
| `PT_LOAD` segments | Memory to allocate, with read, write and execute per segment. |
| Dynamic symbols | Named entry points, which is how kernels are found (section 5). |

So an ordinary ARM or PowerPC ELF, built with no OpenMulticore knowledge at
all, already tells the loader its core kind and most of its feature needs.
What it doesn't say is how it starts and how it reaches the OS: a Linux ARM
executable has the right `e_machine` and the wrong everything else. That is
what the face note is for.

**What OpenMulticore adds: one note.** A `PT_NOTE` segment (and, for linkers
that keep sections, a `.note.openmulticore` section) holding ELF notes whose
owner name is `"OpenMulticore"`. The name scopes the type numbers, so they
can't collide with anyone else's. Note types:

| Type | Name | Holds |
| --- | --- | --- |
| 1 | `OMC_NT_PLACE` | The program's placement: shape, requirements, preferences (section 3.2). |
| 2 | `OMC_NT_KERNELS` | A table of kernels: symbol, core kind, features, the job kind they're submitted as (section 5). |
| 3 | `OMC_NT_FALLBACK` | The ordered list of what to do if no core fits (section 7). |
| 4 | `OMC_NT_FACE` | Which face the program is written to: OpenMulticore native, WarpOS (`powerpc.library`), PowerUP (`ppc.library`), and later `arm.library`, `x86.library`. |
| 5 | `OMC_NT_SIGNATURE` | 5 October 2026: our Kitchen's signature over a native section, which the loader checks before it runs one (section 10). |

**The face note is required for hosting.** The loader hosts an ELF only if it
carries an `OMC_NT_FACE` note naming a face that is present. An ELF without
one is not an OpenMulticore program: the hook passes it to the original
`LoadSeg` (which refuses it, as today) and `OMCRun` says so in a requester.
With the face note and nothing else, the program is placed from the table
above: hosted on a core of its `e_machine`, with the default fallbacks.

We don't need a custom program header type: `PT_NOTE` is found from the
program headers without section headers, so a stripped binary keeps it. If a
later version needs something `PT_NOTE` can't carry, the OS-specific range
(`PT_LOOS` to `PT_HIOS`) is where it would go.

### 3.1 Note layouts

All four notes are longwords in the file's own byte order (always big-endian
in a hunk file, section 4). Each descriptor except `OMC_NT_PLACE` starts with
a version longword, 1 for the layouts here; a loader ignores a note whose
version it doesn't know, and treats it as absent. Strings are NUL-terminated
in a string table at the end of the descriptor and named by their byte offset
from the descriptor's start.

**`OMC_NT_PLACE` (1)** is the tag list below.

**`OMC_NT_KERNELS` (2):** version, entry count, entry size in bytes (40 for
version 1; a larger size means later fields a version 1 loader skips), then
the entries, then the string table. An entry:

| Offset | Field |
| --- | --- |
| `+$00` | NAME: string offset, the name programs look up (`"fft_1024"`) |
| `+$04` | KIND: core kind, as `OMCP_KIND` |
| `+$08` | MINMODEL: as `OMCP_MINMODEL`, 0 for any |
| `+$0C` | NEEDS: standard feature bits, as `OMCP_NEEDS` |
| `+$10` | WANTS: as `OMCP_WANTS` |
| `+$14` | LEVEL: least ISA level (section 9), 0 for any |
| `+$18` | JOBKIND: the job record's kind (`DESIGN.md` section 3), or the maths batch kind |
| `+$1C` | TARGET: for code in an ELF, the string offset of its symbol; in a hunk file, the hunk number; for a host-native or batch kernel, its function number |
| `+$20` | TARGET2: in a hunk file, the offset in that hunk; otherwise 0 |
| `+$24` | VENDOR: 0, or the manufacturer and product (high and low word) whose private bits NEEDS uses (section 9) |

**`OMC_NT_FALLBACK` (3):** version, step count, then one longword per step in
order: 1 `OTHERIMAGE`, 2 `CPU0`, 3 `HOST`, 4 `WAIT`, 5 `REFUSE`, 6 `CORE68K`
(section 7).
A step a loader doesn't know is skipped.

**`OMC_NT_FACE` (4):** version, face, least face version. Faces: 1
OpenMulticore native, 2 WarpOS (`powerpc.library`), 3 PowerUP (`ppc.library`),
4 `arm.library`, 5 `x86.library`. The face version is the library version the
program was built against; an older face present counts as absent.

### 3.2 The placement note

The descriptor is a list of tag and value pairs, two longwords each, ended by
tag 0, in the file's own byte order. Tags a loader doesn't know are skipped, so
the note can grow.

| Tag | Name | Value |
| --- | --- | --- |
| 1 | `OMCP_SHAPE` | 0 anchored, 1 offloading, 2 hosted |
| 2 | `OMCP_KIND` | A core kind, as `DESIGN.md` numbers them (1 68k, 2 PowerPC, 3 ARM, 4 x86, 5 host-native, 6 RISC-V). Defaults to the one `e_machine` implies. Since 5 October the kinds boards offer are 1 (every core, the ISAS m68k bit), 4 as x86-64 and 3 as ARM64; 5 (host-native) is a native section on such a core. |
| 3 | `OMCP_MINMODEL` | The least model: $060 for a 68k, a PVR family for a PowerPC, and so on |
| 4 | `OMCP_NEEDS` | Feature bits the code can't run without (section 9) |
| 5 | `OMCP_WANTS` | Feature bits it runs faster with, used to rank cores |
| 6 | `OMCP_EXCLUSIVE` | 1: the core runs nothing else while this program does |
| 7 | `OMCP_STACK` | Stack bytes on the core |
| 8 | `OMCP_MEMFLAGS` | Extra memory flags for its segments and grants |
| 9 | `OMCP_SAMEBOARD` | 1: all its cores on one board (one cache domain) |
| 10 | `OMCP_MAXCORES` | How many cores its kernels may spread across; 0 for no limit |
| 11 | `OMCP_LEVEL` | Least ISA level (section 9) |
| 12 | `OMCP_VENDOR` | Manufacturer and product (high and low word) whose private feature bits `OMCP_NEEDS` and `OMCP_WANTS` use; without it, those bits must be 0 to 15 only |

## 4. Hunk executables, and modules

Classic 68k programs keep the hunk format; nothing here makes them rebuild.

**A module is a hunk program with native sections in it** (5 October 2026):
its hunks are the 68k section, which every core and every Amiga can run, and
each native section is an ELF image in an `"OMC1"` debug block (type
`OMC_NT_KERNELS` for kernels, or a whole program's image), with the kernel
table naming what is in which section. `LoadSeg` skips debug blocks, so a
module runs as a plain 68k program on a stock OS 3.2 or 3.1 machine with no
OpenMulticore at all. A native section carries a signature from our Kitchen
(a further note, `OMC_NT_SIGNATURE`, type 5); the loader ignores a native
section without a valid one while section 10's signing rule stands, and the
module still runs from its 68k section.

- **Unmarked**, which is every one that exists today: anchored on CPU0. The
  framework never moves a program that didn't ask.
- **Marked**: the same notes as an ELF file (section 3.1), each carried in a
  `HUNK_DEBUG` block whose first longword is `"OMC1"`, followed by the note
  type, the descriptor's length in bytes and the descriptor, big-endian. A
  hunk program has no ELF header to imply a kind, so its `OMC_NT_PLACE` says
  `OMCP_KIND` 1 (68k). `LoadSeg` skips debug hunks, so a marked program still
  loads and runs on a stock OS 3.2 or 3.1 machine with no OpenMulticore at all.
  A tool, `OMCMark`, adds or changes the block in an existing file without
  relinking.
- **Kernels** in a hunk program are other-architecture code it carries: either
  ELF images in a debug block of their own (type `OMC_NT_KERNELS`, then the
  ELF), or files beside the program (section 8). 68k kernels for an extra 68k
  core are ordinary code in the program's own hunks, named in the kernel
  table by hunk number and offset.
- **WarpOS mixed binaries** keep working as they do: their PowerPC hunks
  (`HUNK_PPC_CODE`) are already understood by `powerpc.library`, and the
  WarpOS face places them on a PowerPC core.

A 68k program **hosted** on an extra 68k core (its whole run, not its
kernels) is possible in principle: give the core a proxy `ExecBase` whose
vectors post requests to the anchor. But 68k programs read and write OS
structures directly (`SysBase->ThisTask`, `IntuitionBase` fields, message
ports), and those reads would see stale or wrong memory. It stays out of the
first version; section 10 asks whether to pursue it.

## 5. Kernels: placement inside a program

An offloading program names its kernels instead of choosing cores itself:

```
k = OMC_FindKernel(app, "fft_1024");   /* from the kernel table */
id = OMC_SubmitKernel(k, &args);       /* the framework picks the core */
```

Each kernel entry gives its symbol, its core kind, its needs and wants, and
the job kind it runs as (1 code, 2 host-native function, the maths batch
kind). One kernel may have several entries, one per core kind (`fft_1024` for
ARM with NEON, for a 68060, and as host-native function 14), and the
framework takes the best one present. That is how a single program uses a
PowerPC card on one machine, host cores in AmigaChrome on another, and CPU0
alone on a plain A1200, without any code that asks which is which.

`OMC_Submit(core, &job)` from `DESIGN.md` stays for programs that want to
choose.

## 6. Overrides: settings, tooltypes and the command line

The same keywords in all three places:

| Keyword | Meaning |
| --- | --- |
| `OMC_CORE` | `ANY`, a kind (`68K`, `PPC`, `ARM`, `X86`, `HOST`), or a core number |
| `OMC_PIN` | `YES`: that core or nothing; otherwise a preference |
| `OMC_FALLBACK` | Replaces the binary's fallback list (section 7) |
| `OMC_EXCLUSIVE` | As the tag |
| `OMC_MAXCORES` | As the tag |
| `OMC_OFF` | `YES`: run anchored on CPU0, ignore everything else, as long as the program has a 68k image that CPU0 can run (section 7's `CPU0` test); otherwise it is refused |

- **Settings:** `ENVARC:OpenMulticore/Apps/<name>`, where `<name>` is the
  program's file name. An installer may write one; the user may edit it.
- **Tooltypes:** in the program's icon, or a project icon's.
- **Command line:** `OMCRun [CORE=k] [PIN] [FALLBACK=list] program [args]`.
  `OMCRun` is also how a program is placed on a system without the loader
  hook (section 8).

An override that asks for a core the code can't run on (`OMC_CORE=ARM` on a
PowerPC ELF) is an error with a requester, never a silent change.

## 7. When the core isn't there

The binary lists its fallbacks in order; the user's `OMC_FALLBACK` replaces
the list. Each step is tried until one fits:

| Step | Name | What happens |
| --- | --- | --- |
| `OTHERIMAGE` | Another image | The program ships an image for a kind that is present (section 8): use that. |
| `CORE68K` | A board's 68k core | The 68k section, or a 68k kernel, on a board's core (every core offers `cpu.m68k/1`, a JIT-style translated 68k). Its model, features and level must be met, as for `CPU0`. |
| `CPU0` | Run on CPU0 | The program has a 68k image or 68k kernels whose model, features and level CPU0 meets (read from `AttnFlags` and the library's own CPU check): run them on the motherboard CPU. A 68k image that needs a 68060 or an FPU that CPU0 lacks doesn't qualify, and the step is skipped. |
| `HOST` | Host-native | Kept for old notes: a kernel's host-native function number on a host core. Since 5 October native work is a native section, so this is `OTHERIMAGE` on an x86-64 or ARM64 core. |
| `WAIT` | Wait | Every fitting core is busy or latched: queue until one frees, or until the latch drops. |
| `REFUSE` | Refuse | A requester says what's missing in plain words ("Foo needs a PowerPC core. None is fitted."), and in AmigaChrome where to fit one (the instance's Hardware panel). |

The default list is `OTHERIMAGE, CORE68K, CPU0, REFUSE` (5 October 2026: a
native section on a matching core, else the 68k section on a board core,
else on the main CPU). A module always has its 68k section, so for a module
the list never reaches `REFUSE` unless that section needs what CPU0 lacks
(a 68060, an FPU). `WAIT` is never a
default, because a program waiting on a latch nobody drops looks hung.

The library never runs the code on an unsuitable core and never starts a
program partially placed: either every requirement is met, or the fallback
runs, or the program doesn't start.

## 8. The loader

Two ways in, sharing one loader inside `openmulticore.library`
(`OMC_LoadApp`, `OMC_UnloadApp`, `OMC_AppInfo`):

- **The hook (transparent).** When the library is first opened it patches
  `dos.library`'s `LoadSeg`, `NewLoadSeg` and `InternalLoadSeg` with
  `SetFunction`, as PowerUP's loader did, and stays in memory while the patch
  is in. Something must open it before the first placed program is launched:
  `C:OMCStart`, which the installer adds to `S:Startup-Sequence` after
  `BindDrivers` and ahead of `S:User-Startup` and anything that starts
  programs, opens the library, runs the provider drivers in `SYS:Expansion`
  (`DESIGN.md` section 2), and keeps the library open. A file starting `$7F 'E' 'L' 'F'`, or a hunk file
  with an `"OMC1"` block, goes to `OMC_LoadApp`; everything else goes to the
  original call untouched. For a hosted program it returns a segment list
  holding the 68k anchor, so the Shell, Workbench, `Run` and `WBStartup` all
  start it the normal way and never know. On AROS, where `LoadSeg` already
  loads ELF, the hook passes on only unmarked ELF of CPU0's own `e_machine`.
  A native ELF that carries OpenMulticore notes still goes to `OMC_LoadApp`,
  which loads it with AROS's own loader and then registers its placement and
  kernel table, so `OMC_FindKernel` works for it.
- **`OMCRun` (explicit).** The same loader, called by a command, for systems
  where the user doesn't want `dos.library` patched.

Loading a hosted program: read the headers and notes; choose the image and
the core (section 7 if none fits); allocate its `PT_LOAD` segments as grants;
relocate; build the anchor; ring the core with a kind 1 job whose ENTRY is
the program's entry and whose first argument is the physical address of a
startup grant. The core never sees an OS object: the anchor keeps its reply
port, its `Process` and the Workbench startup message, and copies into the
startup grant what the program needs from them (the command line, the
program's name, Workbench arguments as names) together with opaque handles
the program passes back in its requests (one for the anchor, one per
Workbench argument's lock). The face turns those requests into OS calls on
CPU0. The program's end
is a completion; the anchor returns its code to the Shell.

**Several images in one program.** Since 5 October the module (one file,
section 4) is the form; the drawer below stays readable for programs that
ship that way. A program built for more than one kind can ship as:

- **A drawer** (`Foo.omc/`): `Foo` (68k hunk, the anchor or the CPU0
  version), `Foo.ppc`, `Foo.arm`, `Foo.x86`, each ELF. No new file format; any
  archiver and any file manager handle it. `Foo.omc/Foo` is what the icon
  starts.
- **One file**: the 68k hunk program with each other image in its own
  `"OMC1"` debug block. One file to copy, still runnable everywhere as 68k.

Section 10 asks which to make the default.

**Where it's recorded.** `OMC_AppInfo(anchor, &info)` returns the shape, the
image chosen, the cores in use and which fallback step (if any) applied, so a
tool can show "Foo: PowerPC, core 2" and a program can tell how it was
placed.

## 9. Changes to DESIGN.md

**5 October 2026:** on ACSV boards a core says what it has through its
board, not per-core registers: ISAS gives the instruction sets, and each
service in the board's directory carries a manifest (instruction sets,
memory, FPU, SIMD level such as SSE4.2, AVX2 or NEON) that the library reads
with its name. Placement matches a module's `OMCP_NEEDS` and `OMCP_LEVEL`
against that manifest; a board's 68k cores have ORIGIN 2 (emulated) and its
native cores ORIGIN 0. The registers below are the 4 October proposal for
the earlier board.

Placing by feature needs each core to say what it has. The board-level
`FEATURES` register doesn't, and placement also wants to know whether a core
is real silicon or emulated, so the core block gains three registers in its
unused space:

| Offset | Register | |
| --- | --- | --- |
| `+$2C` | CAPS | Feature bits: 0 FPU, 1 MMU, 2 AltiVec, 3 VFP, 4 NEON, 5 SSE2, 6 AVX2, 7 64-bit mode, 8 RISC-V vector, 9 to 15 reserved, 16 to 31 the maker's own |
| `+$30` | LEVEL | The ISA level where one exists: the x86-64 level, the ARM architecture version, the PowerPC ISA version, the RISC-V profile |
| `+$34` | ORIGIN | 0 native silicon, 1 FPGA, 2 emulated (an interpreter or JIT on another CPU), 3 host (a host thread running host-native functions) |

A provider driver reports the same three values through its calls. When
several cores fit, placement prefers native, then FPGA, then emulated, before
it looks at how busy they are; an emulated 68k core is still a 68k core, so a
program that only fits a 68k runs on one when nothing better is there.

Bits 0 to 15 mean the same on every board. Bits 16 to 31 mean whatever the
board's maker says, so they only match when the program names that maker:
a requirement or kernel that uses them carries `OMCP_VENDOR` (or the kernel
entry's VENDOR), and the library compares those bits only on cores of a board
with that manufacturer and product. Elsewhere such a requirement doesn't fit.

A board whose `VERSION` minor is below 1 reads as `CAPS` 0 and `LEVEL` 0, and
the library falls back to `KIND` and `MODEL` alone and takes the origin as
native. `OMC_CoreInfo` returns
both. The library adds `OMC_LoadApp`, `OMC_UnloadApp`, `OMC_AppInfo`,
`OMC_FindKernel` and `OMC_SubmitKernel`.

## 10. Decisions

We approved this framework on 4 October 2026 without ruling on these
separately, so each stands at its proposal below until we say otherwise.

1. **The loader hook on by default?** Patching `LoadSeg` is what makes a
   launched program go where it needs to without the user doing anything. The
   proposal is on by default, with `OMCRun` for those who turn it off.
2. **Drawer or one file for multi-image programs?** Settled on 5 October
   2026 by cores by instruction set: one file, the module (section 4); the
   drawer stays readable.
3. **Hosted 68k programs on extra 68k cores?** The proposal is kernels and
   `OMC_Run68k` jobs only for 68k in the first version (the board's
   `cpu.m68k/1`), and hosted 68k as a later experiment.
4. **The `CAPS`, `LEVEL` and `ORIGIN` registers**, as section 9 sets them
   out, as OpenMulticore 1.1.
5. **`SYS:Expansion` as the home of every core source** (`DESIGN.md` section
   2). We decided this on 4 October 2026; since 5 October each source is a
   board carrying the ACSV block.
6. **Native code from the Amiga.** A native section is machine code for the
   host, supplied by an Amiga program. The proposal: accept only sections our
   Kitchen built and signed, until native jobs run in a helper process with
   no files, no network and no other system calls (a seccomp sandbox), seeing
   only the job's buffers; then signed or not, by the user's setting.

## 11. Phases

Placement lands alongside the phases in `DESIGN.md`:

Revised 5 October 2026 to `DESIGN.md` section 7's phases.

| With phase | Placement delivers | Done when |
| --- | --- | --- |
| 1 | `OMC_Run68k` and 68k kernels on the board's 68k cores, CPU0 as the fallback | A 68k kernel runs on a board core, and on CPU0 when the board is removed |
| 2 | Modules: the kernel table, native sections in `"OMC1"` blocks, `OMCMark`, `OMC_SubmitKernel`, the default fallback list | One module's kernel runs on a board core or on CPU0, whichever is there |
| 3 | Native x86-64 sections, signed, in the sandboxed helper; the manifest match | A module's x86-64 kernel runs on AmigaChrome's PC and its 68k kernel elsewhere |
| 4 | Native ARM64 sections on the Pi | The same module's ARM64 kernel runs on the Pi |
| Later | The loader hook, `OMCRun` and anchors for hosted native programs; PowerPC faces if PowerPC cores come | A hosted native program started from Workbench runs on its core, and from its 68k section without one |
