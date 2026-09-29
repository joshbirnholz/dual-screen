// SPDX-License-Identifier: GPL-3.0-or-later
//
// A libretro core made of RSDuck's Switch port of melonDS -- the one that draws
// a DS's 2D and 3D on the Switch's GPU through deko3d, and plays Mario Kart DS
// at full speed where libretro's melonDS, drawing in software, cannot -- so the
// Switch app can link it in the way it links libretro's melonDS.
//
// Everything here is glue: the emulator is RSDuck's, compiled unchanged from
// its own tree (see Makefile). It stands in for the two things that port's
// own frontend provides and this program has instead -- the GPU setup the
// renderers draw with, and the libretro interface the app talks to -- and it
// never draws to the screen itself: each frame's two screens are copied off
// the GPU and handed over as pixels, like any other core's.
//
// GPL-3.0-or-later, as melonDS is: this is written against melonDS's own
// interfaces and is linked with it, unlike the rest of this repository.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace melonds_deko {

// The libretro log, once the frontend has given one.
void log_info(const std::string& message);
void log_error(const std::string& message);

// The GPU: a deko3d device, the queue the renderers submit to, the memory they
// allocate from and the shaders they draw with. Before melonDS is initialised,
// since its renderers are made along with it. False, with why, if it cannot be
// set up -- not enough memory, or no shaders in the program's romfs.
bool gfx_init(std::string* error);
void gfx_deinit();

// After melonDS finishes a frame: starts the GPU copying it off once drawn,
// hands the images back to the renderer, and puts the frame before it -- by
// now long copied -- into `out`: 256x384, the upper screen above the touch
// screen, XRGB8888. One frame behind, so the CPU never waits on the GPU.
void gfx_read_screens(uint32_t* out);
// Forgets the frame gfx_read_screens() is holding, so the next one it hands
// over is the one just run rather than one from before a load or a reset.
void gfx_reset_readback();

// Save states, which melonDS writes through a file: Platform::OpenLocalFile()
// and Platform::OpenFile() answer this name with a stream in memory instead.
extern const char* const kStateStreamName;
// Where the next state saved goes, and how much room it has.
void state_target(uint8_t* buffer, std::size_t capacity);
// What the next state loaded is read from.
void state_source(const uint8_t* data, std::size_t size);

// Where BIOS and firmware are looked for, besides a path given in full: the
// frontend's system directory first, then where melonDS's own Switch builds
// keep them.
void set_system_directory(const std::string& directory);

}  // namespace melonds_deko
