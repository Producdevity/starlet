#pragma once

#include <string>
#include <vector>

// Compiles and links, on the current GLES context, every program the renderer generates outside
// draws (texture copies over all their variants, display copies, presentation, clears and
// format changes, bloom, snapshots, offscreen passes, write-backs) on both shading paths, and
// every program of the given program lists. 0 when all of them build.
int check_shaders(const std::vector<std::string>& lists);
