#pragma once

#include <string>
#include <vector>

namespace starlet::dvd {

// One disc of the game as setup installed it: sys/ (boot.bin, fst.bin) and the file roots its FST
// paths are looked up in, in order. A later disc's root usually lists only the files that differ
// from the first disc, followed by the first disc's root.
struct Disc {
    std::string sys;
    std::vector<std::string> roots;
};

// Serves the discs through the DVD SDK calls, starting with discs[0] in the drive. Each disc's file
// table keeps the disc's own entry numbers, offsets and lengths; reads run in order on one drive
// thread and complete as interrupts. The first disc's FST is copied below the arena's top, where the
// apploader leaves it (DVDGetFSTLocation), sized for the largest table. Needs starlet::os set up.
void initialize(const std::vector<Disc>& discs);

// The disc in the drive, 0 for the first.
int current_disc();

}
