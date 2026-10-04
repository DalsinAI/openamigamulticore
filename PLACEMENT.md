<!-- OpenMulticore placement framework, written 4 October 2026. Copyright (c) 2026 Dalsin Limited, MIT licence (LICENSE). -->

# Placing applications on cores

How a program that is launched (from the Shell, Workbench, `Run` or
`WBStartup`) gets to the core it needs, and what happens when that core isn't
there. It sits on `DESIGN.md`: the registers, records and library calls there
are unchanged except where section 9 says so.

Dale, 4 October 2026: "the multicore design must have a framework described to
enable launched applications to go where they need to, ELF should let us do
quite a lot here."

Status, 4 October 2026: specified and approved, nothing built. The choices
in section 10 stand at their proposals.

## 1. What "placing" means under one Exec

The rule from `DESIGN.md` stands: one Exec, on CPU0. Every launched program
therefore starts as an ordinary Exec process on CPU0, and placing it means
deciding which core runs **its code**, not where its process lives. There are
three shapes, and every program is one of them:

| Shape | Process | Code runs on | Who it's for |
| --- | --- | --- | --- |
| **Anchored** | on CPU0 | CPU0 | Every program today, and every unmarked hunk executable. Nothing changes for it. |
| **Offloading** | on CPU0 | CPU0, plus named kernels on other cores | A 68k program that sends maths, codecs or loops to other cores through `OMC_Submit`. The framework places each kernel. |
| **Hosted** | an anchor on CPU0 | another core, start to finish | A PowerPC, ARM or x86 program (or, later, a marked 68k one) whose OS calls come back to its anchor as completion requests, the WarpOS model. |

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

ELF carries most of what placement needs before we add anything, which is why
it's the format of choice for code that runs off CPU0.

**What ELF already says.** The loader reads these without any help from the
program's author:

| Field | Tells the loader |
| --- | --- |
| `e_machine` | The instruction set: `EM_68K` (4), `EM_PPC` (20), `EM_ARM` (40), `EM_AARCH64` (183), `EM_386` (3), `EM_X86_64` (62). This alone picks the core kind. |
| `EI_CLASS`, `EI_DATA` | 32 or 64-bit, and byte order. A little-endian core (ARM, x86) sharing big-endian structures with 68k code is the face's business, not the program's; the loader records the order so the face knows. |
| `e_type` | `ET_DYN` (position-independent) or `ET_REL` (an object, as PowerUP used): the loader relocates it into a grant. `ET_EXEC` with fixed addresses is refused unless its `PT_LOAD` addresses happen to fall in memory the loader can allocate there. |
| `e_flags` | Per-architecture ABI bits: ARM EABI version and float ABI, PowerPC's embedded flags. |
| `.ARM.attributes` | `Tag_CPU_arch`, `Tag_FP_arch`, `Tag_Advanced_SIMD_arch`: which ARM, whether VFP, whether NEON. Compilers emit it. |
| `.gnu.attributes` (PowerPC) | `Tag_GNU_Power_ABI_FP` and the vector ABI: hard or soft float, AltiVec. |
| `.note.gnu.property` (x86) | `GNU_PROPERTY_X86_ISA_1_NEEDED`: the x86-64 level (baseline, v2, v3, v4) the code was built for. |
| `PT_LOAD` segments | Memory to allocate, with read, write and execute per segment. |
| Dynamic symbols | Named entry points, which is how kernels are found (section 5). |

So an ordinary ARM or PowerPC ELF, built with no OpenMulticore knowledge at
all, already tells the loader its core kind and most of its feature needs.

**What OpenMulticore adds: one note.** A `PT_NOTE` segment (and, for linkers
that keep sections, a `.note.openmulticore` section) holding ELF notes whose
owner name is `"OpenMulticore"`. The name scopes the type numbers, so they
can't collide with anyone else's. Note types:

| Type | Name | Holds |
| --- | --- | --- |
| 1 | `OMC_NT_PLACE` | The program's placement: shape, requirements, preferences (section 3.1). |
| 2 | `OMC_NT_KERNELS` | A table of kernels: symbol, core kind, features, the job kind they're submitted as (section 5). |
| 3 | `OMC_NT_FALLBACK` | The ordered list of what to do if no core fits (section 7). |
| 4 | `OMC_NT_FACE` | Which face the program is written to: OpenMulticore native, WarpOS (`powerpc.library`), PowerUP (`ppc.library`), and later `arm.library`, `x86.library`. |

A program without the note is placed from the table above alone: hosted on a
core of its `e_machine`, through the native face, with the default fallbacks.

We don't need a custom program header type: `PT_NOTE` is found from the
program headers without section headers, so a stripped binary keeps it. If a
later version needs something `PT_NOTE` can't carry, the OS-specific range
(`PT_LOOS` to `PT_HIOS`) is where it would go.

### 3.1 The placement note

The descriptor is a list of tag and value pairs, two longwords each, ended by
tag 0, in the file's own byte order. Tags a loader doesn't know are skipped, so
the note can grow.

| Tag | Name | Value |
| --- | --- | --- |
| 1 | `OMCP_SHAPE` | 0 anchored, 1 offloading, 2 hosted |
| 2 | `OMCP_KIND` | A core kind, as `DESIGN.md` numbers them (1 68k, 2 PowerPC, 3 ARM, 4 x86, 5 host-native). Defaults to the one `e_machine` implies. |
| 3 | `OMCP_MINMODEL` | The least model: $060 for a 68k, a PVR family for a PowerPC, and so on |
| 4 | `OMCP_NEEDS` | Feature bits the code can't run without (section 9) |
| 5 | `OMCP_WANTS` | Feature bits it runs faster with, used to rank cores |
| 6 | `OMCP_EXCLUSIVE` | 1: the core runs nothing else while this program does |
| 7 | `OMCP_STACK` | Stack bytes on the core |
| 8 | `OMCP_MEMFLAGS` | Extra memory flags for its segments and grants |
| 9 | `OMCP_SAMEBOARD` | 1: all its cores on one board (one cache domain) |
| 10 | `OMCP_MAXCORES` | How many cores its kernels may spread across; 0 for no limit |

## 4. Hunk executables

Classic 68k programs keep the hunk format; nothing here makes them rebuild.

- **Unmarked**, which is every one that exists today: anchored on CPU0. The
  framework never moves a program that didn't ask.
- **Marked**: the same tag list as the ELF note, carried in a `HUNK_DEBUG`
  block whose first longword is `"OMC1"`, followed by note type, length and
  the tags, big-endian. `LoadSeg` skips debug hunks, so a marked program still
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
| `OMC_OFF` | `YES`: run anchored on CPU0, ignore everything else, as long as the program has a 68k image |

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
| `CPU0` | Run on CPU0 | The program has a 68k image or 68k kernels: run them on the motherboard CPU. Slower, always available. |
| `HOST` | Host-native | A kernel has a host-native function number and a host-native core exists (AmigaChrome): run that. |
| `WAIT` | Wait | Every fitting core is busy or latched: queue until one frees, or until the latch drops. |
| `REFUSE` | Refuse | A requester says what's missing in plain words ("Foo needs a PowerPC core. None is fitted."), and in AmigaChrome where to fit one (the instance's Hardware panel). |

The default list is `OTHERIMAGE, CPU0, HOST, REFUSE`. `WAIT` is never a
default, because a program waiting on a latch nobody drops looks hung.

The library never runs the code on an unsuitable core and never starts a
program partially placed: either every requirement is met, or the fallback
runs, or the program doesn't start.

## 8. The loader

Two ways in, sharing one loader inside `openmulticore.library`
(`OMC_LoadApp`, `OMC_UnloadApp`, `OMC_AppInfo`):

- **The hook (transparent).** At startup the library patches `dos.library`'s
  `LoadSeg`, `NewLoadSeg` and `InternalLoadSeg` with `SetFunction`, as
  PowerUP's loader did. A file starting `$7F 'E' 'L' 'F'`, or a hunk file
  with an `"OMC1"` block, goes to `OMC_LoadApp`; everything else goes to the
  original call untouched. For a hosted program it returns a segment list
  holding the 68k anchor, so the Shell, Workbench, `Run` and `WBStartup` all
  start it the normal way and never know. On AROS, where `LoadSeg` already
  loads ELF, the hook takes only ELF whose `e_machine` isn't the CPU0's own.
- **`OMCRun` (explicit).** The same loader, called by a command, for systems
  where the user doesn't want `dos.library` patched.

Loading a hosted program: read the headers and notes; choose the image and
the core (section 7 if none fits); allocate its `PT_LOAD` segments as grants;
relocate; build the anchor; ring the core with a kind 1 job whose ENTRY is
the program's entry and whose arguments carry the command line, the anchor's
reply port and a pointer to the Workbench startup message. The program's end
is a completion; the anchor returns its code to the Shell.

**Several images in one program.** A program built for more than one kind
can ship as:

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

Placing by feature needs each core to say what it has. The board-level
`FEATURES` register doesn't, so the core block gains two registers in its
unused space:

| Offset | Register | |
| --- | --- | --- |
| `+$2C` | CAPS | Feature bits: 0 FPU, 1 MMU, 2 AltiVec, 3 VFP, 4 NEON, 5 SSE2, 6 AVX2, 7 64-bit mode, 8 to 15 reserved, 16 to 31 the maker's own |
| `+$30` | LEVEL | The ISA level where one exists: the x86-64 level, the ARM architecture version, the PowerPC ISA version |

A board whose `VERSION` minor is below 1 reads as `CAPS` 0 and `LEVEL` 0, and
the library falls back to `KIND` and `MODEL` alone. `OMC_CoreInfo` returns
both. The library adds `OMC_LoadApp`, `OMC_UnloadApp`, `OMC_AppInfo`,
`OMC_FindKernel` and `OMC_SubmitKernel`.

## 10. Decisions

Dale approved this framework on 4 October 2026 without ruling on these
separately, so each stands at its proposal below until he says otherwise.

1. **The loader hook on by default?** Patching `LoadSeg` is what makes a
   launched program go where it needs to without the user doing anything. The
   proposal is on by default, with `OMCRun` for those who turn it off.
2. **Drawer or one file for multi-image programs?** The proposal is the
   drawer first, because it needs nothing new, and the one-file form once
   `OMCMark` exists.
3. **Hosted 68k programs on extra 68k cores?** The proposal is kernels only
   for 68k in the first version, and hosted 68k as a later experiment.
4. **The `CAPS` and `LEVEL` registers**, as section 9 sets them out, as
   OpenMulticore 1.1.

## 11. Phases

Placement lands alongside the phases in `DESIGN.md`:

| With phase | Placement delivers | Done when |
| --- | --- | --- |
| 1 | The kernel table and `OMC_SubmitKernel` for host-native kernels; `OMCMark` for hunk programs | A marked 68k program's maths kernel runs on a host core, and on CPU0 when the board is removed |
| 2 | 68k kernels on extra 68k cores; `CAPS` and `LEVEL` | The same program picks a 68060-model core for a kernel that needs the FPU |
| 3 | The loader hook, `OMCRun`, anchors; hosted PowerPC ELF; the WarpOS and PowerUP faces through it | A PowerPC ELF started from Workbench runs on the PowerPC core, and refuses cleanly without one |
| 5 | Hosted ARM and x86 ELF | As each core kind appears |
