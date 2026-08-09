#ifndef RTS_STATE_H
#define RTS_STATE_H

#include <nds/ndstypes.h>

// Experimental RTS / save states (see docs/rts-architecture.md).
// V0: single slot, cardenginei (DSi/3DS) NTR-mode retail games only.

// Mailbox commands (sharedAddr[4], same protocol as the other IGM FourCCs)
#define RTS_CMD_SAVE 0x56415353 // 'SSAV'
#define RTS_CMD_LOAD 0x444F4C53 // 'SLOD'

// Result codes reported by ARM7 in sharedAddr[3] after a RTS command
#define RTS_OK             0
#define RTS_ERR_NO_FILE    1 // state file missing/cluster not passed
#define RTS_ERR_IO         2
#define RTS_ERR_NO_STATE   3 // file exists but holds no valid state
#define RTS_ERR_WRONG_GAME 4
#define RTS_ERR_VERSION    5
#define RTS_ERR_CRC        6

#define RTS_MAGIC          0x5353424E // 'NBSS'
#define RTS_FORMAT_VERSION 0

// Section IDs
#define RTS_SEC_MRAM 0x4D41524D // 'MRAM' main RAM 0x02000000 (game arena)
#define RTS_SEC_WRK9 0x394B5257 // 'WRK9' 0x027E0000 work/DTCM-mapped window
#define RTS_SEC_CPU9 0x39555043 // 'CPU9' ARM9 context (M3+)
#define RTS_SEC_CPU7 0x37555043 // 'CPU7' ARM7 context (M3+)

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
