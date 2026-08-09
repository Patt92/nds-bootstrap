// Experimental RTS / save states, ARM7 side (see docs/rts-architecture.md).
// M1: state-file plumbing + header write/read-back. No memory or CPU state yet.
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

static tNDSHeader* rtsNdsHeader(void) {
	return (tNDSHeader*)((valueBits & isSdk5) ? NDS_HEADER_SDK5 : NDS_HEADER);
}

static rtsStateHeader stateHeader;

// sharedAddr[3] result for the IGM
static u32 rtsCheckFile(void) {
	if (rtsFileCluster == 0 || rtsFileCluster == 0xFFFFFFFF || rtsFile.firstCluster == CLUSTER_FREE) {
		return RTS_ERR_NO_FILE;
	}
	return RTS_OK;
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
	stateHeader.sectionCount = 0;
	fileWrite((char*)&stateHeader, &rtsFile, 0, sizeof(stateHeader));

	// M2+: CPU contexts, RAM, VRAM, hardware sections get serialized here

	stateHeader.valid = 1;
	fileWrite((char*)&stateHeader, &rtsFile, 0, sizeof(stateHeader));

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

	// M4+: restore pipeline runs here

	sharedAddr[3] = RTS_OK;
}

#endif // !TWLSDK
