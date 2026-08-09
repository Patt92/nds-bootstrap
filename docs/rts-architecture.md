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
7. **Games with the ARM7 IRQ stack in main RAM**: the load path live-restores MRAM while the
   ARM7 still runs C code on its current stack. If a game's ARM7 stack is in main RAM instead
   of ARM7-WRAM, the restore clobbers it mid-execution. Mitigation if it bites: check the
   captured `sp` ranges and defer the affected MRAM stripe to the trampoline.
8. **IRQ window during menu exit on load**: between `leaveCriticalSection()` in the engines'
   menu exit and the trampolines' `IME=0`, an IRQ can run game handlers against restored RAM
   with pre-restore hardware state. Usually survivable (same game code), but a known
   raciness — tighten by masking IME across the whole load exit if it shows up in testing.
9. **Shared WRAM**: only the slice below the ce7/cheat footprint (0x400 bytes) is
   live-restored; the full 32 KiB is captured in the file for later use.

## 11. Status & how to test

Implemented on this branch: M0-M4 (M4 = experimental resume; video/audio/timers/DMA are NOT
restored yet, so post-resume glitches are expected — M5+ is the hardening phase).

Build: needs devkitARM (`dkp-pacman -S nds-dev`, plus `gcc lzss.c -o /usr/local/bin/lzss`),
then `make package-nightly` — or push the branch to a GitHub fork and let the upstream
workflow (`devkitpro/devkitarm:20241104`) build it. **This branch has not been compiled yet**
(no toolchain on the dev machine) — expect a round of compile fixes, and watch the IGM
overlay link (39K region) and the ce7/ce9 region budgets.

On hardware (3DS, TWiLight Menu++, RE:DS):
1. First boot creates `sd:/_nds/nds-bootstrap/states/<TID>-<CRC>.ss0` (8 MiB;
   `SAVE_STATES = 0` in nds-bootstrap.ini disables the feature).
2. M2 check: save a state, corrupt some bytes via the IGM RAM viewer, load — the game session
   is then torn (expected pre-M5), but load must report success and CRCs must hold.
3. M3 check: open/close the menu repeatedly — capture must never destabilize a session.
4. M4 check: save, play 30 s, load — program flow must jump back (screen may glitch until
   M6). After a freeze, the last stage marker is at file offset 0x38 (`stageMarker`).
