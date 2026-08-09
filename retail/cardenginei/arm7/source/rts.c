// Experimental RTS / save states, ARM7 side (see docs/rts-architecture.md).
// Serializes RAM/WRAM/TCM images and both CPU contexts; on load it restores
// them (TCMs and ARM7-WRAM via staging) and arms the resume trampolines
// that longjmp both CPUs back into the save-time context. Hardware state
// beyond IME/IE is not reconstructed yet (M5+), so graphics/audio glitches
// after a resume are expected for now.
#ifndef TWLSDK

#include <nds/ndstypes.h>
#include <string.h>

#include "locations.h"
#include "my_fat.h"
#include "nds_header.h"
#include "rts_state.h"
#include "tonccpy.h"

#define isSdk5 BIT(13)

extern vu32* volatile sharedAddr;
extern u32 valueBits;
extern u32 rtsFileCluster;

extern aFile rtsFile;

extern void inGameMenu(void);
extern int rtsCaptureContext(u32* ctx);

#define REG_IME7 (*(vu32*)0x04000208)
#define REG_IE7  (*(vu32*)0x04000210)

static rtsCpuContext rtsCtx7;

// ARM9 context address in main RAM, published via sharedAddr[2] by the ce9
// menu wrapper before the IGM overlay reports ready; latched by the ARM7
// menu loop so later mailbox traffic (e.g. the RAM viewer) can't clobber it
u32 rtsCtx9Addr = 0;

extern void rtsResumeArm7(u32* ctx, const u32* wramSrc, u32* wramDst, u32 wramLen);

// Armed by a fully validated load; consumed after the menu has exited and
// the ARM7 is back at the hotkey site with the IGM unloaded
static bool rtsResumePending = false;

// Entry point from the hotkey site in cardengine.c. setjmp-style: a zero
// return is the capture, non-zero means the resume trampoline re-entered
// and the VBlank IRQ path must unwind untouched into the restored game.
void rtsMenuArm7(void) {
	if (rtsCaptureContext((u32*)&rtsCtx7) == 0) {
		rtsCtx7.ime = REG_IME7;
		rtsCtx7.ie = REG_IE7;
		inGameMenu();

		if (rtsResumePending) {
			rtsResumePending = false;
			rtsResumeArm7((u32*)&rtsCtx7,
				(const u32*)RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_WRM7_OFFSET),
				(u32*)0x03800000, RTS_WRM7_SIZE);
			// never reached
		}
	}
}

// The ARM7 reads/writes the game's memory through the uncached 0x0C000000
// window (same as dumpRam), sidestepping any question of what the 0x02000000
// arena looks like from the ARM7 bus.
#define RTS_RAM_WINDOW(addr) ((u8*)((addr) - 0x02000000 + 0x0C000000))

typedef struct {
	u32 fourcc;
	u32 targetAddr;
	u32 size;
} rtsRamRange;

// V0 memory policy (retail NTR game):
// - MRAM: the game's 4 MiB arena.
// - WRK9: the 0x027E0000 work window (SDK<5 DTCM mapping/work area), ending
//   at 0x027FC000 so the cardengine, mailbox, header and bootstrap params
//   are never captured or restored (see docs/rts-architecture.md §4).
static const rtsRamRange rtsRamRanges[] = {
	{ RTS_SEC_MRAM, 0x02000000, 0x400000 },
	{ RTS_SEC_WRK9, 0x027E0000, 0x1C000 },
};
#define RTS_RAM_RANGE_COUNT (sizeof(rtsRamRanges) / sizeof(rtsRamRanges[0]))

static rtsStateHeader stateHeader;
static rtsSection sectionTable[RTS_MAX_SECTIONS];

static tNDSHeader* rtsNdsHeader(void) {
	return (tNDSHeader*)((valueBits & isSdk5) ? NDS_HEADER_SDK5 : NDS_HEADER);
}

// CRC32 (IEEE, reflected), 16-entry table to keep the ce7 footprint small
static u32 rtsCrc32(u32 crc, const u8* data, u32 len) {
	static const u32 table[16] = {
		0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
		0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
		0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
		0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
	};

	crc = ~crc;
	while (len--) {
		crc ^= *data++;
		crc = table[crc & 0xF] ^ (crc >> 4);
		crc = table[crc & 0xF] ^ (crc >> 4);
	}
	return ~crc;
}

static u32 rtsCheckFile(void) {
	if (rtsFileCluster == 0 || rtsFileCluster == 0xFFFFFFFF || rtsFile.firstCluster == CLUSTER_FREE) {
		return RTS_ERR_NO_FILE;
	}
	return RTS_OK;
}

// RTS_DEBUG: persist the current stage so a hard freeze on real hardware is
// localizable after a reboot (header is rewritten, valid flag untouched)
static void rtsSetStage(u32 stage) {
	stateHeader.stageMarker = stage;
	fileWrite((char*)&stateHeader, &rtsFile, 0, sizeof(stateHeader));
}

void rtsSaveState(void) {
	u32 res = rtsCheckFile();
	if (res != RTS_OK) {
		sharedAddr[3] = res;
		return;
	}

	const tNDSHeader* header = rtsNdsHeader();

	// Invalidate first so a torn write never looks like a loadable state
	toncset(&stateHeader, 0, sizeof(stateHeader));
	stateHeader.magic = RTS_MAGIC;
	stateHeader.formatVersion = RTS_FORMAT_VERSION;
	stateHeader.valid = 0;
	tonccpy(stateHeader.gameCode, header->gameCode, 4);
	stateHeader.headerCRC = header->headerCRC16;
	rtsSetStage(RTS_STAGE_HEADER);

	toncset(sectionTable, 0, sizeof(sectionTable));
	u32 fileOffset = RTS_PAYLOAD_OFFSET;
	u32 sectionCount = 0;

	// CPU contexts and the ARM9-staged TCM images. The IGM staged DTCM/ITCM
	// into the ext region and flushed its caches before issuing this command.
	const struct {
		u32 fourcc;
		const u8* src;   // ARM7-visible source
		u32 targetAddr;  // ARM9-side home of the data
		u32 size;
	} fixedSections[] = {
		{ RTS_SEC_CPU7, (const u8*)&rtsCtx7, (u32)&rtsCtx7, sizeof(rtsCtx7) },
		{ RTS_SEC_CPU9, RTS_RAM_WINDOW(rtsCtx9Addr), rtsCtx9Addr, sizeof(rtsCpuContext) },
		{ RTS_SEC_DTCM, RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_DTCM_OFFSET), 0, RTS_DTCM_SIZE },
		{ RTS_SEC_ITCM, RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_ITCM_OFFSET), 0, RTS_ITCM_SIZE },
		{ RTS_SEC_WRM7, (const u8*)0x03800000, 0x03800000, RTS_WRM7_SIZE },
		{ RTS_SEC_WRMS, (const u8*)0x03000000, 0x03000000, RTS_WRMS_SIZE },
	};
	for (u32 i = 0; i < sizeof(fixedSections) / sizeof(fixedSections[0]); i++) {
		rtsSection* section = &sectionTable[sectionCount++];
		section->fourcc = fixedSections[i].fourcc;
		section->targetAddr = fixedSections[i].targetAddr;
		section->fileOffset = fileOffset;
		section->size = fixedSections[i].size;
		if (fixedSections[i].fourcc == RTS_SEC_CPU9 && rtsCtx9Addr == 0) {
			section->size = 0; // ce9 never published a context (unexpected)
		} else {
			fileWrite((char*)fixedSections[i].src, &rtsFile, fileOffset, section->size);
			section->crc32 = rtsCrc32(0, fixedSections[i].src, section->size);
		}
		fileOffset += section->size;
	}

	// RAM ranges, written straight from the RAM window
	for (u32 i = 0; i < RTS_RAM_RANGE_COUNT; i++) {
		const rtsRamRange* range = &rtsRamRanges[i];
		rtsSection* section = &sectionTable[sectionCount++];

		section->fourcc = range->fourcc;
		section->targetAddr = range->targetAddr;
		section->fileOffset = fileOffset;
		section->size = range->size;

		rtsSetStage(RTS_STAGE_RAM);
		fileWrite((char*)RTS_RAM_WINDOW(range->targetAddr), &rtsFile, fileOffset, range->size);

		rtsSetStage(RTS_STAGE_CRC);
		section->crc32 = rtsCrc32(0, RTS_RAM_WINDOW(range->targetAddr), range->size);

		fileOffset += range->size;
	}

	fileWrite((char*)sectionTable, &rtsFile, RTS_SECTION_TABLE_OFFSET, sizeof(sectionTable));

	stateHeader.sectionCount = sectionCount;
	stateHeader.valid = 1;
	rtsSetStage(RTS_STAGE_DONE);

	sharedAddr[3] = RTS_OK;
}

void rtsLoadState(void) {
	u32 res = rtsCheckFile();
	if (res != RTS_OK) {
		sharedAddr[3] = res;
		return;
	}

	const tNDSHeader* header = rtsNdsHeader();

	toncset(&stateHeader, 0, sizeof(stateHeader));
	fileRead((char*)&stateHeader, &rtsFile, 0, sizeof(stateHeader));

	if (stateHeader.magic != RTS_MAGIC || stateHeader.valid != 1) {
		sharedAddr[3] = RTS_ERR_NO_STATE;
		return;
	}
	if (stateHeader.formatVersion != RTS_FORMAT_VERSION) {
		sharedAddr[3] = RTS_ERR_VERSION;
		return;
	}
	if (memcmp(stateHeader.gameCode, header->gameCode, 4) != 0
	 || stateHeader.headerCRC != header->headerCRC16) {
		sharedAddr[3] = RTS_ERR_WRONG_GAME;
		return;
	}
	if (stateHeader.sectionCount > RTS_MAX_SECTIONS) {
		sharedAddr[3] = RTS_ERR_VERSION;
		return;
	}

	fileRead((char*)sectionTable, &rtsFile, RTS_SECTION_TABLE_OFFSET, sizeof(sectionTable));

	// The CPU context homes must match this build/session, otherwise the
	// captured stacks cannot unwind (different cardengine layout)
	for (u32 i = 0; i < stateHeader.sectionCount; i++) {
		const rtsSection* section = &sectionTable[i];
		if ((section->fourcc == RTS_SEC_CPU7 && section->targetAddr != (u32)&rtsCtx7)
		 || (section->fourcc == RTS_SEC_CPU9 && (rtsCtx9Addr == 0 || section->targetAddr != rtsCtx9Addr))) {
			sharedAddr[3] = RTS_ERR_VERSION;
			return;
		}
	}

	// Restore every section, then verify the restored bytes. From the first
	// restored section on, the session is committed: on any error the game
	// memory is already a mix of two worlds and only a reset helps. The
	// ARM9 sits in its polling loop (stack in DTCM, code in the IGM region)
	// and invalidates its caches once this command returns; the TCM and
	// ARM7-WRAM images stay staged until the resume trampolines copy them.
	for (u32 i = 0; i < stateHeader.sectionCount; i++) {
		const rtsSection* section = &sectionTable[i];
		if (section->size == 0) {
			continue;
		}

		u8* dst;
		switch (section->fourcc) {
			case RTS_SEC_MRAM:
			case RTS_SEC_WRK9:
			case RTS_SEC_CPU9:
				dst = RTS_RAM_WINDOW(section->targetAddr);
				break;
			case RTS_SEC_CPU7:
				dst = (u8*)&rtsCtx7;
				break;
			case RTS_SEC_DTCM:
				dst = RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_DTCM_OFFSET);
				break;
			case RTS_SEC_ITCM:
				dst = RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_ITCM_OFFSET);
				break;
			case RTS_SEC_WRM7:
				dst = RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_WRM7_OFFSET);
				break;
			case RTS_SEC_WRMS:
				// staged behind the WRM7 image, partially copied below
				dst = RTS_RAM_WINDOW(INGAME_MENU_EXT_LOCATION + RTS_STAGING_WRM7_OFFSET + RTS_WRM7_SIZE);
				break;
			default:
				continue; // unknown section from a newer minor format
		}

		rtsSetStage(RTS_STAGE_RESTORE);
		fileRead((char*)dst, &rtsFile, section->fileOffset, section->size);

		rtsSetStage(RTS_STAGE_VERIFY);
		if (rtsCrc32(0, dst, section->size) != section->crc32) {
			rtsSetStage(RTS_STAGE_DONE);
			sharedAddr[3] = RTS_ERR_CRC;
			return;
		}

		if (section->fourcc == RTS_SEC_WRMS) {
			// Only the game-owned slice below the ce7/cheat footprint is
			// live-restored in V0; the rest is bootstrap-owned right now
			tonccpy((u8*)0x03000000, dst, RTS_WRMS_RESTORE_SIZE);
		}
	}
	rtsSetStage(RTS_STAGE_DONE);

	// Arm the resume: the IGM exits the menu on RTS_OK, the ce9 wrapper
	// jumps through rtsResumeArm9, and rtsMenuArm7 through rtsResumeArm7
	rtsResumePending = true;

	sharedAddr[3] = RTS_OK;
}

#endif // !TWLSDK
