// Experimental RTS / save states, ARM7 side (see docs/rts-architecture.md).
// M2: main-RAM snapshot with CRC verification. The ARM9 IGM flushes its
// caches before issuing a command, so physical RAM is coherent here.
//
// Loading a state only restores RAM so far (no CPU/hardware context yet) -
// the session is NOT expected to survive a load until M4. The point of the
// M2 load path is proving byte-exact restore via CRC.
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
	stateHeader.sectionCount = RTS_RAM_RANGE_COUNT;
	rtsSetStage(RTS_STAGE_HEADER);

	// Section payloads: RAM ranges, written straight from the RAM window
	toncset(sectionTable, 0, sizeof(sectionTable));
	u32 fileOffset = RTS_PAYLOAD_OFFSET;
	for (u32 i = 0; i < RTS_RAM_RANGE_COUNT; i++) {
		const rtsRamRange* range = &rtsRamRanges[i];
		rtsSection* section = &sectionTable[i];

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

	// Restore RAM sections, then verify the restored memory byte-exactly.
	// The ARM9 sits in its polling loop (stack in DTCM, code in the IGM
	// region) and invalidates its caches once this command returns.
	for (u32 i = 0; i < stateHeader.sectionCount; i++) {
		const rtsSection* section = &sectionTable[i];
		if (section->fourcc != RTS_SEC_MRAM && section->fourcc != RTS_SEC_WRK9) {
			continue; // future section types are not restorable by M2 code
		}

		rtsSetStage(RTS_STAGE_RESTORE);
		fileRead((char*)RTS_RAM_WINDOW(section->targetAddr), &rtsFile, section->fileOffset, section->size);

		rtsSetStage(RTS_STAGE_VERIFY);
		if (rtsCrc32(0, RTS_RAM_WINDOW(section->targetAddr), section->size) != section->crc32) {
			rtsSetStage(RTS_STAGE_DONE);
			sharedAddr[3] = RTS_ERR_CRC;
			return;
		}
	}
	rtsSetStage(RTS_STAGE_DONE);

	// M4+: CPU context + hardware restore and resume trampoline run here

	sharedAddr[3] = RTS_OK;
}

#endif // !TWLSDK
