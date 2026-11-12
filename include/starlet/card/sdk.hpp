#pragma once

// starlet::card_sdk: the SDK's CARD calls (starlet/sdk/card.h) for ports on starlet::os.

#include <chrono>
#include <string>

namespace starlet::card {

// Names the saves folder before the game starts. CARDInit then inserts slot A: Dolphin's
// GCI folder for the boot disc's region (boot_info_block), a Memory Card of size_mbits,
// formatted, created if missing; slot B stays empty. An empty saves_dir, or a folder
// that cannot be created, leaves slot A empty and the game plays without saving.
// Called again after CARDInit (tests), it inserts the new folder at once.
void initialize_cards(const std::string& saves_dir, int size_mbits = 16);
// The exit path: waits up to timeout for the card requests already made to reach the
// folder. False when some are still running.
bool finish_card_writes(std::chrono::milliseconds timeout);

}
