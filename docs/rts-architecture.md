# Experimental RTS / Save States — Architecture Analysis (M0)

**Fork base:** upstream `DS-Homebrew/nds-bootstrap` master @ `565f48b79064e7cca89da09c317cdeaa86b7dc2d`
("Do not write to VCOUNT register when resetting or exiting game")
**Branch:** `feature/experimental-rts`
**Scope:** retail NTR-mode games via nds-bootstrap on CFW 3DS/2DS (TWiLight Menu++).
Explicitly out of scope for V0: B4DS, TWLSDK/DSiWare, flashcards, Wi-Fi, Slot-2, multiple slots.
**Reference game:** Resident Evil: Deadly Silence.
**Upstream issue:** https://github.com/DS-Homebrew/nds-bootstrap/issues/143

---

## 1. Which engine actually runs on the target

nds-bootstrap has two engine families:

| Path | Mode | Relevant for V0? |
|---|---|---|
| `retail/cardenginei/*` | DSi/3DS ("i" engine, NTR & TWL) | **Yes** — this is what runs on 3DS |
| `retail/cardengine/*` | B4DS (DS/DS Lite, flashcard) | No |

The original project sketch pointed at `retail/cardengine/arm7/source/inGameMenu.c` etc.
Those are the **B4DS** variants. The files that matter for the 3DS target are:

- `retail/cardenginei/arm7/source/cardengine.c` — ARM7 engine, hotkey detect, **all file I/O**
- `retail/cardenginei/arm7/source/inGameMenu.c` — ARM7 side of the IGM command loop
- `retail/cardenginei/arm9/source/cardengine.c` — ARM9 engine, IPC handler, IGM invocation
- `retail/cardenginei/arm9_igm/` — the IGM overlay binary (runs on ARM9), own Makefile + ld script
- `retail/cardengine/arm9_igm/Makefile` — builds the **B4DS** IGM from the same sources with `-DB4DS`

Both IGM builds compile from `retail/cardenginei/arm9_igm/source/`. Changes there must either be
`#ifndef B4DS`-guarded or kept B4DS-safe; V0 guards all RTS code with `#ifndef B4DS`.

## 2. How the In-Game Menu works today (i-engine)

### 2.1 Trigger and handshake

1. **ARM7** `myIrqHandlerVBlank()` (`cardenginei/arm7/source/cardengine.c:1917`) polls
   `REG_KEYINPUT`/`REG_EXTKEYINPUT` against `igmHotkey`. On match (and `igmAccessible`,
   `tryLockMutex(&saveMutex)`), it calls ARM7 `inGameMenu()`.
   → **ARM7 is inside its VBlank IRQ handler for the entire menu session.**
2. ARM7 `inGameMenu()` (`inGameMenu.c:57`):
   - `sharedAddr[4] = 'MENU'`, `IPC_SendSync(0x9)`, `REG_MASTER_VOLUME = 0`,
     `enterCriticalSection()` (ARM7 IME off).
   - `loadInGameMenu()`: pages out `0xA000` bytes of RAM at `INGAME_MENU_LOCATION`
     (`0x02F88000`) to pagefile offset `0xA000`, reads the IGM binary from pagefile offset `0`.
     Guarded by `sharedAddr[5] = 'IGML'`.
   - Waits for ARM9 to report `'REDY'` in `sharedAddr[5]`, then enters the command loop.
3. **ARM9** receives IPC sync `0x9` in `myIrqHandlerIPC()` (`cardenginei/arm9/source/cardengine.c:1705`)
   → ARM9 `inGameMenu()` (line 1544): waits for the `'IGML'` load to finish,
   `enterCriticalSection()`, optionally `WRAM_CR = 0` (grabs shared WRAM), then **calls the IGM
   overlay entry** at `INGAME_MENU_LOCATION + IGM_ENTRY`.
   → **ARM9 is inside the game's IRQ dispatch (hooked IRQ table) for the entire menu session.**
4. The IGM overlay (`arm9_igm`) draws on the sub screen, backs up/restores DISPCNT_SUB, BGxCNT_SUB,
   VRAM_C/H mapping, POWERCNT, palettes, BG map/tiles — and *cannot* back up write-only regs
   (`inGameMenu.c:896-899`: `REG_MOSAIC_SUB`, `REG_BLDCNT_SUB`, `REG_BLDALPHA_SUB`, `REG_BLDY_SUB`
   are simply zeroed, with the comment "Register is write only, can't back up").
5. On exit: ARM9 IGM returns a result code; ARM7 `unloadInGameMenu()` writes the IGM back to
   pagefile offset `0` and restores game RAM from offset `0xA000`; both sides
   `leaveCriticalSection()`; ARM7 restores `REG_MASTER_VOLUME = 127`.

### 2.2 Command protocol (shared memory mailbox)

`sharedAddr` = `CARDENGINE_SHARED_ADDRESS_SDK5` (`0x02FFFA0C`) or `_SDK1` (`0x027FFA0C`);
the IGM overlay receives the pointer via a word written at `INGAME_MENU_LOCATION + IGM_TEXT_SIZE_ALIGNED`.

| Word | Use during IGM |
|---|---|
| `sharedAddr[0..2]` | command arguments (address, value, index…) |
| `sharedAddr[3]` | result/status (e.g. `'RAMD'` while dumping, reset param) |
| `sharedAddr[4]` | **command mailbox**: ARM9 IGM writes a FourCC, ARM7 executes and writes back `'MENU'` |
| `sharedAddr[5]` | key state broadcast (ARM7→ARM9); also `'IGML'` / `'REDY'` handshake |
| `sharedAddr[6]` | battery/brightness/volume |
| `sharedAddr[7..8]` | RTC time + receive flag |

Existing FourCCs: `MENU EXIT RSET QUIT IPCS FPSA RAMD SSPP SHOT PMAN MANU RMAN RAMR RAMW LITE VOLU IGML REDY`.
RTS adds new FourCCs in the same mailbox — no parallel IPC architecture.

**Important asymmetry:** the ARM9 IGM is the *UI + ARM9-side memory access*; **every SD-card
access happens on ARM7** (`fileRead`/`fileWrite` on `aFile` handles opened via
`getFileFromCluster`). RTS follows the same split: ARM9 = capture/restore of ARM9-owned state
into RAM-visible staging + cache management; ARM7 = serialization to SD + ARM7-owned state.

### 2.3 Pagefile layout (`sd:/_nds/pagefile.sys`, 6 MiB, created by loader `conf_sd.cpp`)

| Offset | Size | Content |
|---|---|---|
| `0x000000` | `0xA000` | IGM overlay binary (written by loader at boot, incl. IgmText + font) |
| `0x00A000` | `0xA000` | game RAM backup under the IGM while menu is open |
| `0x014000` | ~2.7 MB | ARM9 binary backup (soft-reset path) |
| `0x2BFE00` | `0x160` | NDS header backup |
| `0x2C0000` | — | ARM7 binary backup |
| `0x540000` | `0x40000` | `INGAME_MENU_EXT_LOCATION` backup (screenshot/manual scratch) |
| `0x5FFFF0` | 16 | binary sizes |

There is **no slack** for a 4 MiB+ state → RTS gets its **own file** (see §7).

### 2.4 IGM overlay constraints

- Linked to `INGAME_MENU_LOCATION = 0x02F88000`, **region length 39K** (`cardengine.ld.in`),
  paged in/out as one `0xA000` block. RTS UI code must stay small; heavy lifting lives in
  ce7/ce9, not in the overlay.
- First `0xF40` bytes are `struct IgmText` (`common/include/igm_text.h`), **filled by the loader**
  (`conf_sd.cpp` → `getIgmStrings`) — a fixed ABI (`static_assert(sizeof == 0xF40)`), and
  `menu[8][20]` is fully used by the 8 existing entries. → V0 hardcodes the strings
  `"Save State"` / `"Load State"` inside the overlay instead of growing IgmText (avoids
  touching the loader/translation pipeline; localization can come later).
- Useful helpers already in the overlay:
  - `cpsr.s` → `getCPSR`
  - `dcache.s` → `DC_FlushRange`, `DC_InvalidateAll`, `DC_InvalidateRange`
  - `card_engine_header.s` → `changeMpu`/`revertMpu` (opens MPU regions 0/2/3 for full-range
    access — used by the RAM viewer; **directly reusable for RTS dumps**), `codeJump` (jump
    to arbitrary code via literal), IGM entry saves `r2-r11,lr` only.
  - `biosCalls.s`, `exception.c` (register-dump UI — a ready-made display for RTS debug).

### 2.5 Existing RAM dump (reuse target)

ARM7 `dumpRam()` (`cardenginei/arm7/source/cardengine.c:939`): triggered by `'RAMD'`,
writes `0x0C000000` (uncached/extended mirror of main RAM in TWL mode) for
`0x01000000`/`0x02000000` bytes (DSi/3DS) to `ramdump.bin` via `fileWrite` — proof that
**multi-MiB ARM7 SD writes from inside the frozen-IGM state already work**. Note it dumps the
*16/32 MiB physical* RAM through the `0x0C000000` window, which bypasses the ARM9 cache
question for ARM7 — but **not** the dirty-line problem (see §5.3).

## 3. CPU state at the freeze point

### 3.1 Where the game actually is when the IGM is open

Both engines hook the **SDK's IRQ jump table** (`bootloaderi/source/arm7/hook_arm7.c`,
`hook_arm9.c`; `ce9->irqTable`/ce7 equivalents):

- ARM7: game IRQ dispatch → hooked VBlank vector → `myIrqHandlerVBlank` → `inGameMenu()` loop.
- ARM9: game IRQ dispatch → hooked IPC-sync vector → `myIrqHandlerIPC` → `inGameMenu()` →
  overlay entry.

So at the moment the menu is open, on **both** CPUs the interrupted game context consists of:

1. Registers the BIOS IRQ entry stacked on the **IRQ stack** (`r0-r3, r12, lr_irq`), where
   `lr_irq - 4` = interrupted PC.
2. `SPSR_irq` = interrupted CPSR (mode, Thumb bit, flags, I/F bits).
3. Callee-saved registers `r4-r11`, `sp/lr` of the interrupted mode — still live in the register
   file / stacked by the SDK dispatcher and our own C prologues.
4. Whatever the SDK's `OS_IrqHandler` wrapper additionally stacked (SDK-version-dependent; may
   switch to system mode for nesting).

**Consequence for M3:** capture must happen in a dedicated assembly entry *before* deep C call
chains, reading: current regs, `SPSR_irq`, banked `sp/lr` of SYS/user mode (`ldm ^` or mode
switch), and it must record CPSR/mode so we can verify at runtime which mode the SDK dispatcher
actually uses (display via IGM, RE:DS first). We do **not** need to unwind to the exact
interrupted instruction by parsing frames: restoring *RAM + IRQ stacks + banked regs + SPSR* and
re-running the same IRQ-return path the dispatcher would have taken reproduces the interrupted
context, because the full return state lives in memory we snapshot — **provided the stacks are
inside captured memory** (see DTCM below!).

### 3.2 Memory that holds that context — and is NOT main RAM

| Memory | CPU | Size / location | In `0x02000000` dump? |
|---|---|---|---|
| ARM9 **DTCM** | ARM9 only | 16 KiB, SDK maps at `0x027E0000` (SDK<5) / `0x02FFC000`-ish (SDK5) — **CPU-internal**, ARM7-invisible | **No** — separate section, dumped by ARM9 |
| ARM9 **ITCM** | ARM9 only | 32 KiB @ `0x01000000` mirror — holds SDK fast code + game IRQ vectors | **No** — separate section, dumped by ARM9 |
| ARM7 WRAM | ARM7 only | 64 KiB @ `0x037F8000` | **No** — ARM7 dumps its own; **contains ce7 + cheat engine → partial exclusion** |
| Shared WRAM | bankable | 32 KiB @ `0x03000000` + `WRAM_CR` | **No** — dump from owning CPU + save `WRAM_CR` |

The SDK places IRQ/SVC stacks for ARM9 in **DTCM**. A "main RAM only" snapshot therefore cannot
resume — this is a hard requirement for M3/M4, not an optimization.

### 3.3 Caches (ARM9)

ARM7 sees physical RAM; ARM9 runs write-back D-cache. Ordering rules:

- **Save:** ARM9 must `DC_FlushAll` (clean+invalidate or at least clean) *after* the freeze
  handshake and *before* ARM7 serializes main RAM. ce9 already has `cacheFlush()`
  (`exceptionHandler.s` / `dcache.s`).
- **Load:** after RAM restore (by ARM7 or ARM9), ARM9 must invalidate D-cache and I-cache
  before jumping into restored code (`memcpy + goto saved_pc` is not enough).
- The IGM overlay's MPU trampolines (`changeMpu`) already deal with the "IGM data must be
  reachable uncached/unrestricted" problem for the RAM viewer.

## 4. RAM ownership map (what must NOT be blindly restored)

nds-bootstrap co-resides with the game inside the 16 MiB TWL RAM. Restoring bootstrap-owned
memory to an older snapshot desynchronizes the engine from reality (open files, ROM cache
descriptors, IGM paging state). Exclusion list for the restore path (i-engine, retail NTR game):

| Range | Owner | Policy |
|---|---|---|
| `0x02000000-0x02400000` | game (4 MiB retail) | **save + restore** (minus holes below) |
| `0x02F88000 + 0xA000` | IGM overlay (while menu open: menu code; game RAM is in pagefile `0xA000`) | game bytes come from pagefile during save; on load they are written back **through the pagefile slot**, not the live region (§6) |
| `sharedAddr[0..8]` (`0x02FFFA0C+`/`0x027FFA0C+`) | mailbox | **never restore** — it is the live comm channel |
| `0x027FC000` / SDK5 locations (ce9), `0x026B8000` ext, `0x026F0000+` buffers, `0x0C000000+` ROM cache & tables (`0x027D8000-0x027E6000`) | bootstrap | **exclude**; ROM cache stays warm — reads after load simply hit/miss normally |
| ARM7 WRAM `0x037F8000-0x03800000` minus ce7 (`0x037E0400+` is shared-WRAM side; actual ce7 at `CARDENGINE*_ARM7_LOCATION`) and cheat engine (`0x037DC000`) | split | ARM7 saves/restores the **game-owned** part only |
| NDS header / `0x02FFE000` DSi header region | mixed (bootstrap patches it) | save, restore **except** bootstrap-patched words — needs a per-word diff list in M4 hardening |

(Exact SDK5-vs-SDK<5 windows get finalized in M2 against RE:DS, which is SDK2-era; the state
format's section table makes the split explicit per game.)

## 5. Hardware state

### 5.1 Register catalog approach

Full catalog is M5+ work (tracked in `docs/rts-hw-registers.md`, to be created with GBATEK +
libnds `video.h`/`sound.h` as references). Non-negotiable classes already known:

- **Readable & restorable:** DISPCNT/BGxCNT, DISPSTAT (partially), VRAMCNT A-I, WRAM_CR, IE/IME,
  timers' control+counter (counter reads live value), DMA SAD/DAD/CNT (readable on NDS unlike GBA),
  IPC FIFO CNT, POWERCNT, SOUNDCNT master.
- **Write-only (the issue #143 problem):** `BGxHOFS/BGxVOFS`, `REG_MOSAIC`, `BLDCNT/BLDALPHA/BLDY`
  (both engines), 3D GXFIFO command stream, SPU per-channel `SOUNDxTMR`/`SOUNDxPNT`, `IF`
  (write-to-clear), DISPCAPCNT partially, various latches.
- **Side-effect-on-access:** IPC FIFO (read pops), GXSTAT/GXFIFO, `IF` (write clears), sound
  capture.

Mitigation ladder (V0 order): (A) restore everything readable; (B) shadow bootstrap's *own*
writes; (C) shadow via existing SDK-function patches where a game's misbehavior proves it
necessary; (D) targeted instrumentation — explicitly *not* V0.

The precedent for (B) already exists upstream: `scfgExtBak`/`scfgClkBak` in the IGM header are
exactly such software shadows for SCFG registers.

### 5.2 Ordering rules

- Save: **capture registers first, then quiesce** (stop DMA, disable timers via control after
  reading control+counter, mask IRQs after reading IE/IF) — never the reverse.
- Load: restore memory → devices with IRQ/timer/DMA *disabled* → IE (IF handled via
  acknowledge semantics) → CPU contexts → IME/CPSR-I last, inside the resume trampoline.
- The IGM session itself already perturbs hardware (MASTER_VOLUME=0, POWERCNT swap, sub-screen
  video regs, WRAM_CR=0 if `useSharedWram`, SetBrightness, write-only sub-blend regs zeroed).
  → RTS capture happens at **menu-entry time as far as possible**, and the state format records
  what the IGM already clobbered so V0 can document (not yet fix) those losses. Long-term fix:
  move capture of the affected registers to the freeze handshake *before* the IGM touches them.

### 5.3 DMA / card engine interaction

`saveMutex` already ensures the IGM doesn't open during a card-read/save; the freeze point is
after any in-flight ce9 card DMA completed (`myIrqHandlerIPC` runs the DMA completion first).
V0 policy: capture DMA registers, stop channels, on load re-arm from captured registers;
document games where mid-transfer resume misbehaves. No cycle-exact DMA reconstruction.

## 6. The self-overwrite problem (restore code vs. restored memory)

Actors during load, and where they live:

- ARM7 code (ce7): ARM7 WRAM — *not* overwritten by main-RAM restore. ARM7 can restore all of
  main RAM via the `0x0C000000` window without shooting itself.
- ARM9 IGM overlay at `0x02F88000`: inside restored space. Solution = existing paging design:
  the `0xA000` game bytes for that window are restored **into the pagefile slot at `0xA000`**;
  the normal `unloadInGameMenu()` then writes them to live RAM after the overlay is done.
- ce9 + shared mailbox: excluded from restore (§4).
- Final ARM9 resume: small trampoline copied to a safe scratch (ITCM tail or the IGM region
  *after* its job is done — decided in M4), which restores banked regs/SPSR/regs and returns
  with interrupts still off; nothing C runs after it on the restored world.

Save is symmetric: the `0xA000` under the IGM is read **from the pagefile**, not from live RAM.

## 7. State file & plumbing

### 7.1 File

`sd:/_nds/nds-bootstrap/states/<TID>-<headerCRC>.ss0` (one slot, V0), created/sized by the
loader at boot like `screenshots.tar` (no TWiLight Menu++ changes needed), cluster passed
loader → `load_crt0` → bootloaderi → new `rtsFileCluster` field in ce7 header
(**both** `cardengine_header_arm7.h` *and* `card_engine_header.s` — fixed parallel layouts!).
Size budget V0: 4 MiB main RAM + 656 KiB VRAM + 96+32 KiB WRAM + 48 KiB TCM + 4 KiB OAM/PAL
+ headers/regs → **8 MiB** file (`0x800000`) leaves room for SDK5 windows later.

### 7.2 Format (M2)

```
[0x000] RtsHeader: magic 'NBSS', formatVersion, flags, valid,
        gameCode+headerCRC, bootstrap version string + base commit,
        sectionCount, payloadCrc32, timestamp
[0x040] Section table: {fourcc, targetAddr, fileOffset, size, crc32, flags(cpu, restoreOrder)}
        CPU9 CPU7 MRAM DTCM ITCM WRM7 WRMS VRAM(A-I + VRAMCNT) PALT OAM_ VREG DREG TREG IREG IPCR AREG BSTP
[...]   payloads
```

Write protocol: header `valid=0` → payloads → CRCs → `valid=1` (torn-write safety);
loading validates gameCode+headerCRC+formatVersion+CRC before touching anything.

### 7.3 New mailbox commands (V0)

| FourCC | Direction | Action |
|---|---|---|
| `'SSAV'` | IGM→ARM7 | run save-state pipeline |
| `'SLOD'` | IGM→ARM7 | validate + run load-state pipeline |
| (M3+) `'SFRZ'`/`'SRDY'` | ARM7↔ARM9 | explicit freeze/thaw handshake extensions |

Stage markers (`RTS_DEBUG`): ARM7 writes the current stage FourCC + last CRC to a fixed
offset in the state file header area before each stage, so a hard freeze is localizable
after reboot.

## 8. Save / load pipelines (target design)

Save: IGM open (both CPUs parked, IME off, saveMutex held) → ARM9: capture ARM9 CPU frame,
readable ARM9-side regs, `DC_FlushAll`, dump DTCM/ITCM to staging → ARM7: capture ARM7 frame +
ARM7 regs (SPU, timers7, IE7…) → quiesce → ARM7 serializes: header(valid=0), CPU sections,
MRAM (via `0x0C000000`), WRAM7, staged TCM, VRAM (ARM9 maps banks LCDC and stages, or ARM7
reads staged copies), regs, CRCs, valid=1 → resume via the normal IGM exit path.

Load: validate → quiesce → ARM7 restores MRAM (minus §4 holes; IGM window into pagefile slot) →
TCM/VRAM/palettes/OAM restored by ARM9 from staging → device regs in dependency order (video →
audio → IPC → timers → DMA), IRQ last before CPUs → ARM7 context restore, parks in its IRQ
return → ARM9 trampoline: I/D-cache invalidate, banked regs, SPSR, `subs pc, lr, #4`-style
return into the game. Both sides re-enable IME only through the restored SPSR/IRQ-return.

## 8b. As built (M1-M4, this branch)

The freeze/resume core is implemented as a **setjmp/longjmp pair around the existing menu
entry points** — no new synchronization architecture:

- Capture (`rtsCaptureContext`, identical asm on both CPUs): at the exact IRQ entry into the
  menu path (`ce9` case 0x9 / `ce7` hotkey site), r4-r11, sp/lr, CPSR, the interrupted SPSR and
  banked SYS/SVC state are stored. Caller-saved registers are dead at that call boundary.
  Caller frames above the capture SP stay untouched for the whole menu session, so the
  DTCM/WRAM dumps taken later still describe the capture instant.
- Resume (`rtsResumeArm9`/`rtsResumeArm7`): stack-free asm in the engine regions (excluded
  from restore). Copies the staged TCM/ARM7-WRAM images in, invalidates caches (ARM9),
  restores IE/IME + banked state + SPSR + registers, and returns to the capture site with
  r0=1 — the save-time IRQ path then unwinds through the just-restored stacks into the game.
- Staging map inside `INGAME_MENU_EXT_LOCATION`: WRM7 image at +0x18200 (vramBak slot, load
  only), WRMS behind it, DTCM at +0x34000, ITCM at +0x38000.
- State file sections: CPU7 CPU9 DTCM ITCM WRM7 WRMS MRAM WRK9, each CRC32-verified after
  restore; header `valid` flag written last; stage markers persisted for post-freeze triage.
- ARM9⇄ARM7 handoff on load: IGM auto-exits the menu, hands `RTS_RESUME_MAGIC` to the ce9
  wrapper via `sharedAddr[3]` (the ARM7 exit path leaves that word alone), ARM7 arms its own
  `rtsResumePending` and jumps after `inGameMenu()` returns at the hotkey site.

## 9. Milestones & acceptance (unchanged from project brief)

M0 this document · M1 IGM entries + dummy commands + file plumbing · M2 format + MRAM dump/verify
(CRC before/after, no resume expectation) · M3 CPU capture w/o load · M4 first real
save→run→load→jump-back (graphics may glitch) · M5 IRQ/Timer/DMA/IPC · M6 video/VRAM ·
M7 audio · M8 write-only catalog work.

Acceptance (RE:DS): save mid-gameplay → 30 s play → load → 60 s stable; then
save→resume→load→resume ×10 without reboot; later: load a state after full restart.

## 10. Risks / open questions (tracked)

1. SDK IRQ dispatcher mode/nesting per SDK version — verify at runtime on RE:DS (M3 debug UI).
2. `useSharedWram` games: WRAM_CR is force-set during IGM; capture must happen before, restore after.
3. 3D geometry engine: no full pipeline restore in V0; accept one glitched frame, investigate
   re-init needs if it wedges (GXSTAT/GXFIFO flush before capture).
4. IGM-clobbered write-only sub-engine regs (MOSAIC/BLD*) are lost *today* even without RTS;
   RTS V0 inherits this; fix = capture-at-freeze before IGM draws (M6).
5. Games writing MMIO directly (non-SDK): out of V0 scope by design (mitigation ladder D).
6. `.sav`/`.pub`/`.prv`, cheats, AP patches: untouched by design — state file is separate and
   the save path never writes through the game-save code paths. **A loaded state can still be
   logically inconsistent with the on-cart save file written after the snapshot** — document
   for users (same caveat as emulator save states).
7. **ARM7 memory map (open, gating M4)**: ce7 links at `0x037E0400` (61 KiB) and the cheat
   engine at `0x037DC000` — both in the DSi WRAM window, so restoring ARM7 WRAM at
   `0x03800000` does *not* clobber the running engine. But the game's own ARM7 region at
   `0x037F0000-0x03800000` (right above ce7) sits in that same window and the captured `sp`
   may point into either region. Both would have to be copied from the stack-free trampoline,
   and the staging area in `INGAME_MENU_EXT_LOCATION` only has ~15 KiB spare against 128 KiB
   needed. Until this is measured on hardware, `RTS_RESTORE_ARM7_MEM` is **0**: WRM7/WRA7/WRMS
   are captured into the state file but not restored.
8. **Games with the ARM7 IRQ stack in main RAM**: the load path live-restores MRAM while the
   ARM7 still runs C code on its current stack. If a game's ARM7 stack is in main RAM instead
   of ARM7-WRAM, the restore clobbers it mid-execution. Mitigation if it bites: check the
   captured `sp` ranges and defer the affected MRAM stripe to the trampoline.
9. **IRQ window during menu exit on load**: between `leaveCriticalSection()` in the engines'
   menu exit and the trampolines' `IME=0`, an IRQ can run game handlers against restored RAM
   with pre-restore hardware state. Usually survivable (same game code), but a known
   raciness — tighten by masking IME across the whole load exit if it shows up in testing.
10. **Shared WRAM**: only the slice below the ce7/cheat footprint (0x400 bytes) is
   live-restored; the full 32 KiB is captured in the file for later use.

## 10b. Hardware test 1 (2026-08-09, New 3DS, RE:DS)

**Saving works.** A state is written and the game keeps running afterwards, which
makes M2/M3 (context capture + ~4.5 MiB serialization without destabilizing the
session) plausible on hardware for the first time.

**Loading crashed:** Data Abort, PC `0x020F3FB0` (game code), faulting address `0`,
`r1 = 0x43484152` ("RAHC" — the CHAR section magic of a NITRO graphics resource),
`r2 = 0x6000001F` (a VRAM destination). The game was parsing a graphics resource and
followed a NULL pointer, i.e. it read data that was not what it expected to be there.

**One definite bug found, present in both paths:** the TCM staging area is
`INGAME_MENU_EXT_LOCATION + 0x34000/0x38000` (`0x026EC000`-`0x026F8000`). Upstream
never touches that region without paging it out to `pagefile.sys` first
(`prepareScreenshot()`) and reading it back afterwards (`saveScreenshot()`) — it is
memory the running system owns. The RTS code wrote 48 KiB of TCM images into it with
no page-out, on the save path (ARM9 staging) and on the load path (ARM7 writing the
images back into staging). Anything that later read that memory would see TCM bytes.

**Fixes applied:**
- the save path now brackets staging with the same page-out/page-in
  (`SSPP` before staging, new `REST` command afterwards, plus `DC_InvalidateRange`),
  and flushes the ARM9 cache before the ARM7 reads the region;
- the load path no longer stages anything: `RTS_RESTORE_TCM` is **0**, so TCM images
  are captured into the state file but not restored, and the resume trampoline no
  longer copies them. The load path could not page the region back in anyway — the
  trampoline runs after the menu has already exited.
- a refused command no longer runs `rtsCacheInvalidate()`, which would have dropped
  still-dirty game cache lines without cleaning them first.

**Not established:** whether that bug was *the* cause of the load crash. Two other
load-path problems are still open and either could produce the same symptom:

1. **The ARM9's stack may be inside the live-restored `WRK9` window.** The dump shows
   the game's `sp = 0x027E3AF4`, inside `0x027E0000`-`0x027FC000`. Restoring memory
   the ARM9 is currently executing on violates the rule in §6 and would have to be
   deferred to the trampoline.
2. **The address-window assumption is unverified.** `WRK9`/`MRAM` are read and written
   by the ARM7 through the `0x0C000000` window. Whether the game's `0x027Exxxx`
   accesses and the ARM7's `0x0C7Exxxx` window reach the same physical memory depends
   on the NTR 4 MiB mirror behaviour on DSi/3DS hardware, which this design got wrong
   twice already. It has to be measured, not reasoned about: the IGM RAM viewer can
   settle it by comparing `0x023E3AF4` with `0x027E3AF4` in a running game. If both
   show the same bytes, the mirror is active and every ARM7-side `0x0C7Exxxx` access
   in this code addresses the wrong memory.

Until (2) is answered, further restore work is guesswork.

## 10c. Hardware test 2 (2026-08-09, build f56a4aa) — DTCM identified

**Loading worked once**, when the state was saved and loaded without leaving the
room; every other attempt ends in a Data Abort. That pattern is the diagnosis.

Second dump: PC `0x0210958C`, faulting address `0`, `sp = 0x027E3A24`,
`r3 = 0x027E4000`, `r8 = 0x04001000` and `r12 = 0x04000000` (both display engine
I/O bases). First dump: `sp = 0x027E3AF4`. Both stack pointers sit in
`0x027E0000`-`0x027E4000` and `r3` points exactly at the end of that range — that is
the **ARM9 DTCM**, at the DS SDK's usual mapping, 16 KiB.

**What that means:**

- The game's ARM9 stack lives in DTCM. The captured context's `sp`/`lr` therefore
  point into DTCM, and DTCM is *not* restored (`RTS_RESTORE_TCM 0`). After a load the
  CPU resumes onto a stack whose contents are from *now* instead of from save time.
  Save and load in the same spot → the stack happens to hold nearly the same frames →
  the resume survives. Change rooms in between → the frames differ → the return path
  unwinds into the wrong place. **Restoring DTCM is the missing piece for M4**, not
  video state.
- The removed `WRK9` section (`0x027E0000` + `0x1C000`) started exactly at the DTCM
  base. DTCM is CPU-internal and invisible to the ARM7, so that section never
  captured DTCM; the `0x0C7E0000` window it used addresses an unrelated physical
  location, into which every load wrote 112 KiB. Removed, and the format version
  bumped to 1 so older state files are rejected.

**Still blocking, and now the single most important open question:** whether the NTR
4 MiB view is mirrored. Every ARM7-side access goes through
`RTS_RAM_WINDOW(addr) = addr - 0x02000000 + 0x0C000000`, which is only correct for
addresses inside the first 4 MiB. The staging slots
(`INGAME_MENU_EXT_LOCATION + 0x34000/0x38000` = `0x026EC000`/`0x026F8000`) are above
that. If the mirror is active they alias into the first 4 MiB and the window
addresses used for them are wrong — which would mean the DTCM/ITCM images in the
state file are garbage, and that the staging writes hit the wrong memory. Both
readings of upstream's behaviour (pagefile page-out of `INGAME_MENU_EXT_LOCATION`)
are consistent with either answer, so it has to be measured.

The build therefore prints `RAM MIRROR: YES/NO/?` on the save/load result screen: it
compares three spread-out windows of live game RAM against their `+4 MiB` aliases,
read-only, with the MPU opened the same way the RAM viewer does it. `?` means every
probed word was zero, i.e. inconclusive.

Once that is answered:
- **mirror active** → fix `RTS_RAM_WINDOW` to mask into the 4 MiB arena, then restore
  DTCM from the trampoline (16 KiB, staged after the MRAM restore so it survives it,
  with an ARM9→ARM7 handshake to repair the 16 KiB of game RAM underneath).
- **flat** → the window is already correct, and DTCM restore only needs the
  trampoline copy plus the page-out bracket the save path already uses.

## 10d. Hardware test 3 (2026-08-09, build 35614cb)

**Loading is partially working.** A load moved the character three steps back, in the
same room and field of view — the restore genuinely takes effect. Moving out of the
current view or changing rooms before loading gives either a crash or graphics
garbage.

Third dump: PC `0x02078320`, faulting address `0`, `sp = 0x027E3F80` — DTCM again,
this time near the top of it, i.e. an almost empty stack. The stack words below hold
CPSR-shaped values (`0x200000B2` = IRQ mode, Thumb, IRQs masked) and return addresses
in `0x027Fxxxx`, so the exception unwound to close to the base of an IRQ frame.

The DTCM conclusion from §10c stands and is now the top priority. But the same
arithmetic surfaced a second, independent problem:

**The MRAM section overlapped the cardengine.** `MRAM` covered
`0x02000000`-`0x02400000`. With the NTR 4 MiB view mirrored — which is what makes the
game's own `0x027FFE00` header accesses work — `0x023FC000`-`0x02400000` is the same
memory as `0x027FC000`-`0x02800000`, which holds ce9 (12 KiB), the shared mailbox
(`0x027FFA0C`), the unpatched-function table, the exception stack and the NDS header.
Every load therefore overwrote the running cardengine and the mailbox, mid-protocol,
with save-time bytes. ce9's code is identical between save and load so this often
survives, but its variables — including the captured ARM9 context itself — are in
that range. `MRAM` is now `0x400000 - 0x4000`, i.e. the top 16 KiB is excluded. If
the view turns out to be flat instead, the cost is at most 16 KiB of unrestored game
state.

**The mirror question is still unanswered and now gates the DTCM work too.** The
staging slots used for the TCM images (`0x026EC000`/`0x026F8000`) are above 4 MiB, so
`RTS_RAM_WINDOW` is only correct for them if the view is flat. The planned fix is to
move staging *below* 4 MiB, which is unambiguous either way: the ARM7 restores MRAM,
writes the DTCM image into a 16 KiB slice of it, the trampoline copies that slice into
DTCM, and an ARM9→ARM7 handshake then repairs the slice from the MRAM section in the
state file before either CPU resumes. That design does not depend on the answer — but
whether `MRAM` may also need further exclusions does.

The build prints `RAM MIRROR: YES/NO/?` on the **save** result screen (not the load
one — a successful load leaves the menu immediately).

## 10e. Hardware test 4 — mirror measured, DTCM restore implemented

**The RAM MIRROR probe reports NO, consistently.** The NTR view is flat: the game's
`0x027Exxxx`/`0x027Fxxxx` addresses are real memory distinct from the first 4 MiB.
Consequences, all now settled:

- `RTS_RAM_WINDOW(addr) = addr - 0x02000000 + 0x0C000000` is a straight offset and is
  correct for every address this code uses, including the staging slots above 4 MiB.
  The DTCM images already in the state files were therefore valid all along.
- `MRAM` (`0x02000000` + 4 MiB) does **not** alias ce9, the mailbox or the NDS header
  at `0x027Fxxxx`. The top-16 KiB exclusion from §10d was introduced on the mirror
  hypothesis the measurement refutes, so it has been taken back out — keeping it
  would only have dropped 16 KiB of game state for no reason.

**Load result before this change:** the game came back **not frozen**, ran, and the
graphics repaired themselves after a room change; audio was noise. So the remaining
gaps are exactly the ones the milestone plan predicts — VRAM (M6) and SPU (M7) — plus
the DTCM stack problem from §10c.

**DTCM restore, now implemented.** The ext region is paged out to `pagefile.sys`
before both commands (as the screenshot path does). On load the ARM7 leaves the DTCM
image in that staging slot instead of consuming it, and `rtsResumeArm9` — stack-free,
because the game's ARM9 stack lives in the DTCM it is about to overwrite — invalidates
the caches, opens MPU region 0, copies the 16 KiB image to the CP15-reported DTCM
base, and then hands the region back:

```
ARM9 trampoline            ARM7 (rtsFinishResume)
  copy staging -> DTCM
  sharedAddr[1] = 'DTCD'  ->
                             page ext region back in from pagefile.sys
                          <- sharedAddr[1] = 'DTCA'
  invalidate caches
  restore context, longjmp   restore context, longjmp
```

Both waits are bounded (ARM9 ~16M iterations, ARM7 ~32M), so a lost handshake
degrades to a bad resume rather than a hung console. ITCM is deliberately not
restored: it holds code, which is identical across a save/load pair.

Verified in the linked object: the literal pool holds `0x026EC000`, `'DTCD'`, `'DTCA'`,
and the copy loop moves `0x4000` bytes in 16-byte blocks.

## 10f. Hardware test 5 — DTCM restore changed nothing

Reported after adding the DTCM restore: **behaviour is identical.** Same hanging
audio, same graphical artefacts, same self-repair on a room change. Nothing improved
and nothing got worse. A further observation: loading after walking to a different
camera frame *in the same room* puts the room's objects at the player's position, and
the camera switch makes the character jump about before settling.

Identical behaviour is itself evidence: if the DTCM copy were running and DTCM
mattered, something should have shifted. Two readings, and they need to be told apart
before any more design work:

1. **The trampoline's DTCM path is not executing** (handshake lost, section absent,
   `getDtcmBase()` wrong) — in which case the idea is untested rather than refuted.
2. **DTCM simply was not the blocker.** The dumps show the game's ARM9 stack near the
   top of DTCM (`sp = 0x027E3F80`, 128 bytes below the end), i.e. a shallow stack. If
   the SDK's DTCM contents barely differ between two moments in the same session,
   restoring them is a no-op in practice, and the state that actually diverges lives
   elsewhere.

The build therefore reports, on the **save** result screen, what the previous load
actually managed:

```
RAM MIRROR:   NO
LAST RESUME:  OK | NO HANDSHAKE | NONE YET
DTCM BYTES:   00004000
```

`OK` means the ARM9 trampoline reported its DTCM copy and the ARM7 paged the staging
region back in. `NO HANDSHAKE` means the ARM7 waited ~33M iterations without the ARM9
ever signalling — the trampoline did not reach that point. `DTCM BYTES` is the size of
the DTCM section the load actually put into staging; `00000000` means the state file
has no usable DTCM section.

**If it reads `OK` / `00004000`, hypothesis 2 holds** and attention moves to what is
still not restored at all. The strongest remaining candidate is **ARM7 state**: the
sound driver and the game's ARM7 side keep running with present-time state while the
ARM9 jumps back, so the two processors are desynchronised — which is exactly what
hanging audio looks like. `WRA7` (`0x037F0000`-`0x03800000`, the game's ARM7 region,
above ce7's 61 KiB at `0x037E0400`) and `WRM7` (ARM7 WRAM at `0x03800000`) are
captured but still not restored; restoring them has the same self-overwrite problem as
DTCM and needs the same trampoline treatment, with staging that survives the pagefile
restore. After that: VRAM/OAM/palettes (M6), which is what the graphical artefacts and
their self-repair on a room change point at.

## 10g. Hardware test 6 — the resume works; VRAM and audio implemented (M6/M7)

Reported: **the state loads correctly.** Program flow, position and game state come
back. What is broken is sprites and sound — and both self-repair: entering and leaving
the settings screen redraws everything correctly, and the music recovers on a room
change. That is precisely the M4-complete / M6-M7-missing picture, and it means the
resume core (context capture, MRAM, DTCM, the trampolines) is done.

**M6, implemented.** The ARM7 cannot read VRAM, and a bank can only be copied while
it is mapped to the CPU, so the transfer is driven from the ARM9 one bank at a time
using the same LCDC trick as the screenshot code: save `VRAMCNT`, write `0x80`
(enable, MST 0), copy through the staging area, put `VRAMCNT` back. A generic
section-transfer pair (`SECW`/`SECR`, fourcc in `sharedAddr[0]`, size in
`sharedAddr[2]`) carries each bank, so the staging area only has to hold the largest
bank (128 KiB) rather than all 656 KiB. Sections: `VRA0`-`VRA8` for banks A-I and
`VMEM` for the palettes (2 KiB), OAM (2 KiB) and the nine `VRAMCNT` bytes. Note
`0x04000247` is WRAMCNT, not a bank — H and I are at `0x248`/`0x249`.

Two ordering traps handled:

- The menu's own cleanup restores `BG_MAP_RAM_SUB`, `BG_PALETTE_SUB`, `BG_GFX_SUB`,
  `VRAM_C_CR` and `VRAM_H_CR` from backups taken *before* the load, which would undo
  part of the restore. The three memory backups are refreshed from the restored VRAM
  so the cleanup becomes a no-op, and the two bank-mapping backups are handed the
  restored values by pointer.
- Each section type now has its own staging slot. The DTCM image has to survive until
  the trampoline reads it, after the menu is gone, so it must not share a slot with
  the bank transfers.

**M7, partially.** The SPU's per-channel source, timer and length registers are
write-only — the classic issue #143 problem — so a resumed channel cannot be told
where it was. All 16 channels are stopped on load instead. That trades hanging noise
for silence until the game restarts its music, which it does on its own.

Save now costs ten extra mailbox round trips and load eleven, roughly a frame each.

## 10h. Hardware test 7 — M6/M7 froze the console, disabled

The VRAM/palette/OAM/SPU work of §10g **froze the console completely**, where the
build before it resumed correctly with broken sprites and sound. `RTS_ENABLE_VIDEO`
is therefore **0**: the default build behaves exactly like 802da05 again, and the code
stays in the tree behind the flag.

That change was too large to ship untested in one step — it added ten mailbox round
trips per save and eleven per load, VRAM bank remapping, a new section protocol and
an SPU write, all at once, with no way to tell them apart from the outcome. Splitting
it is the fix, not more analysis.

Candidates, in the order they should be tested, one at a time:

1. **A deadlock in the new round trips.** `rtsMailbox` spins until the ARM7 hands
   `sharedAddr[4]` back. A complete freeze with no exception screen is what a
   CPU-to-CPU deadlock looks like, and this is the only new place both sides wait.
2. **The SPU write.** `0x04000400` is an ARM7-only register block; the load path
   writes it from the ARM9. That should be ignored rather than fatal, but it is new,
   trivially removable, and untested.
3. **VRAM bank remapping while the menu is on screen.** The overlay draws from bank H
   and the restore switches banks to LCDC underneath it.
4. **The bank table.** A wrong size or LCDC address would copy past a bank's end.

The next step is to re-enable only one of these at a time. The section transfer
protocol itself can be tested without touching video at all: save with it enabled but
restore nothing, which exercises the round trips in isolation.

## 11. Status & how to test

Implemented on this branch: M0-M4 (M4 = experimental resume; video/audio/timers/DMA are NOT
restored yet, so post-resume glitches are expected — M5+ is the hardening phase), plus
configurable quick save/load hotkeys, plus M6 (VRAM/palettes/OAM) and a partial M7.
On hardware the resume itself works; see §10b-§10g.

Build: `make package-nightly` under devkitARM, or the same container CI uses:

```
docker run --rm -v "$PWD":/work -w /work devkitpro/devkitarm:20241104 \
  bash -c 'apt-get update && apt-get install -y gcc && gcc lzss.c -o /usr/local/bin/lzss \
           && git config --global safe.directory "*" && make package-nightly'
```

**Build status: green** on `devkitpro/devkitarm:20241104`. Region budgets as built:
IGM overlay 19052 B of the 39 KiB region (loader copies 0xA000), ce7 49380 B of 61 KiB.
The `LOAD segment with RWX permissions` linker warnings are pre-existing upstream.
**Still never run on hardware.**

Scope guard: `rtsCheckFile()` refuses SDK5 games (`RTS_ERR_UNSUPPORTED`) because the captured
windows (`0x027E0000` work area, cardengine right above it) use the SDK<5 layout.

Hotkeys (all configurable in `nds-bootstrap.ini`, hex key masks, `0` disables):
`SAVE_STATE_HOTKEY = 184` (R+Down+Select), `LOAD_STATE_HOTKEY = 144` (R+Up+Select) —
deliberate mirrors of the in-game menu's `HOTKEY = 284` (L+Down+Select), chosen so that no
combination is a subset of another hotkey (notably `SCREEN_SWAP_HOTKEY = 740`, L+R+X+Up).
Detection is edge-triggered in the ARM7 VBlank handler and takes the same freeze path as the
menu; the overlay runs the command instead of drawing the menu. A zero mask is checked
explicitly — without that, `REG_KEYINPUT & 0` would match every frame.

On hardware (3DS, TWiLight Menu++, RE:DS):
1. First boot creates `sd:/_nds/nds-bootstrap/states/<TID>-<CRC>.ss0` (8 MiB;
   `SAVE_STATES = 0` in nds-bootstrap.ini disables the feature).
2. M2 check: save a state, corrupt some bytes via the IGM RAM viewer, load — the game session
   is then torn (expected pre-M5), but load must report success and CRCs must hold.
3. M3 check: open/close the menu repeatedly — capture must never destabilize a session.
4. M4 check: save, play 30 s, load — program flow must jump back (screen may glitch until
   M6). After a freeze, the last stage marker is at file offset 0x2C (`stageMarker`).
