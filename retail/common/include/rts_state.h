#ifndef RTS_STATE_H
#define RTS_STATE_H

#include <nds/ndstypes.h>

// Experimental RTS / save states (see docs/rts-architecture.md).
// V0: single slot, cardenginei (DSi/3DS) NTR-mode retail games only.

// Mailbox commands (sharedAddr[4], same protocol as the other IGM FourCCs)
#define RTS_CMD_SAVE 0x56415353 // 'SSAV'
#define RTS_CMD_LOAD 0x444F4C53 // 'SLOD'
#define RTS_CMD_EXT_RESTORE 0x54534552 // 'REST' - page the ext region back in

// INGAME_MENU_EXT_LOCATION, used to stage the TCM images, is memory that
// belongs to the running system: upstream never touches it without paging it
// out to pagefile.sys first and reading it back afterwards - see
// prepareScreenshot()/saveScreenshot(). The save path now does the same.
//
// The load path cannot: it would need the staged images to survive until the
// resume trampoline, which runs after the menu has already exited, so there
// is no point left at which the region could be paged back in. It therefore
// does not stage anything at all - the TCMs are captured into the state file
// but not restored, like ARM7 memory above. A resumed ARM9 keeps its current
// TCM contents.
#define RTS_RESTORE_TCM 0

// Result codes reported by ARM7 in sharedAddr[3] after a RTS command
#define RTS_OK             0
#define RTS_ERR_NO_FILE    1 // state file missing/cluster not passed
#define RTS_ERR_IO         2
#define RTS_ERR_NO_STATE   3 // file exists but holds no valid state
#define RTS_ERR_WRONG_GAME 4
#define RTS_ERR_VERSION    5
#define RTS_ERR_CRC        6
#define RTS_ERR_UNSUPPORTED 7 // game layout outside the V0 scope (SDK5)

#define RTS_MAGIC          0x5353424E // 'NBSS'
#define RTS_FORMAT_VERSION 1

// Section IDs
#define RTS_SEC_MRAM 0x4D41524D // 'MRAM' main RAM 0x02000000 (game arena)
#define RTS_SEC_CPU9 0x39555043 // 'CPU9' ARM9 context
#define RTS_SEC_CPU7 0x37555043 // 'CPU7' ARM7 context
#define RTS_SEC_DTCM 0x4D435444 // 'DTCM' ARM9 data TCM (16 KiB, ARM9-staged)
#define RTS_SEC_ITCM 0x4D435449 // 'ITCM' ARM9 instruction TCM (32 KiB, ARM9-staged)
#define RTS_SEC_WRM7 0x374D5257 // 'WRM7' ARM7-exclusive WRAM (64 KiB @ 0x03800000)
#define RTS_SEC_WRMS 0x534D5257 // 'WRMS' shared WRAM as seen by ARM7 (32 KiB @ 0x03000000)
#define RTS_SEC_WRA7 0x37415257 // 'WRA7' game ARM7 region in the DSi WRAM window
                                //        (64 KiB @ 0x037F0000, above ce7's 61 KiB
                                //         at 0x037E0400 and the cheat engine)

// 0x027E0000 is the ARM9's DTCM, not a main-RAM window: both hardware crash
// dumps put the game's stack there (0x027E3AF4 / 0x027E3A24, with r3 pointing
// at the 0x027E4000 DTCM end). DTCM is CPU-internal and invisible to the ARM7,
// and the 0x0C7E0000 window an earlier 'WRK9' section used for it addressed an
// unrelated physical location entirely. DTCM is captured through ARM9 staging
// (RTS_SEC_DTCM) instead; the game's own 0x027Exxxx accesses are covered by
// MRAM via the NTR 4 MiB mirror.
//
// The ARM7-side memory map is not settled yet: the captured contexts show
// stacks in both 0x037F0000+ and 0x03800000+ depending on SDK version, and
// both regions would have to be copied from a stack-free trampoline (the
// staging area in INGAME_MENU_EXT_LOCATION only has ~15 KiB spare). Until
// that is verified on hardware, both regions are captured into the state
// file but NOT restored - a resumed ARM7 keeps its current memory.
// See docs/rts-architecture.md section 10.
#define RTS_RESTORE_ARM7_MEM 0

// Written by the IGM into sharedAddr[3] after a successful load so the ce9
// menu wrapper runs the resume trampoline once the menu has fully exited
#define RTS_RESUME_MAGIC 0x454D5352 // 'RSME'

// CPU context captured by rtsCaptureContext at the point where the IRQ
// handler enters the menu path (setjmp-style; see docs/rts-architecture.md
// §3). Caller-saved registers are dead at that call boundary. The first 17
// words are written by assembly and must match its store order; ime/ie are
// filled in by C afterwards (per-CPU MMIO, unreachable from the other CPU).
typedef struct rtsCpuContext {
	u32 r[8];    // r4-r11
	u32 sp, lr;  // capture mode (IRQ path)
	u32 cpsr;    // capture-time CPSR
	u32 spsr;    // interrupted game CPSR
	u32 spSys, lrSys;
	u32 spSvc, lrSvc, spsrSvc;
	u32 ime, ie;
} rtsCpuContext;

// Staging areas inside INGAME_MENU_EXT_LOCATION (0x40000 bytes total;
// 0x0-0x30200 is the screenshot area, unused while a RTS command runs.
// The WRM7 image reuses the vramBak slot at 0x18200 during a load only.)
#define RTS_STAGING_WRM7_OFFSET 0x18200
#define RTS_STAGING_DTCM_OFFSET 0x34000
#define RTS_STAGING_ITCM_OFFSET 0x38000
#define RTS_DTCM_SIZE 0x4000
#define RTS_ITCM_SIZE 0x8000
#define RTS_WRM7_SIZE 0x10000
#define RTS_WRA7_SIZE 0x10000
#define RTS_WRMS_SIZE 0x8000
// Only the slice of shared WRAM below the cardengine/cheat-engine footprint
// is restored in V0 (the rest is bootstrap-owned while a game runs)
#define RTS_WRMS_RESTORE_SIZE 0x400

// Save/load stage markers (RTS_DEBUG, stored in header stageMarker)
#define RTS_STAGE_HEADER  0x30445248 // 'HRD0'
#define RTS_STAGE_RAM     0x314D4152 // 'RAM1'
#define RTS_STAGE_CRC     0x32435243 // 'CRC2'
#define RTS_STAGE_DONE    0x33454E44 // 'DNE3'
#define RTS_STAGE_RESTORE 0x34545352 // 'RST4'
#define RTS_STAGE_VERIFY  0x35465256 // 'VRF5'

// State file size as allocated by the loader (fixed, written in place)
#define RTS_FILE_SIZE 0x800000

#define RTS_MAX_SECTIONS 24

// Written at offset 0 of the state file. valid is 0 while a save is in
// progress so a torn write can never look like a loadable state.
typedef struct rtsStateHeader {
	u32 magic;         // RTS_MAGIC
	u16 formatVersion; // RTS_FORMAT_VERSION
	u16 valid;         // 1 = complete state
	char gameCode[4];  // NDS header offset 0xC
	u16 headerCRC;     // NDS header offset 0x15E
	u16 sectionCount;
	u32 flags;
	u32 timestamp;     // RTC at save time, packed; 0 in V0
	char bootstrapVer[20];
	u32 stageMarker;   // RTS_DEBUG: last stage FourCC before a crash
	u32 reserved[3];
} rtsStateHeader;

// Section table entry; table starts at offset 0x40
typedef struct rtsSection {
	u32 fourcc;
	u32 targetAddr;
	u32 fileOffset;
	u32 size;
	u32 crc32;
	u32 flags; // bit 0: owned by ARM7, bit 1-7: restore order
} rtsSection;

#define RTS_SECTION_TABLE_OFFSET 0x40
#define RTS_PAYLOAD_OFFSET (RTS_SECTION_TABLE_OFFSET + RTS_MAX_SECTIONS * sizeof(rtsSection))

#endif // RTS_STATE_H
