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

#include <deko3d.h>

namespace melonds_deko {

// The libretro log, once the frontend has given one.
void log_info(const std::string& message);
void log_error(const std::string& message);
void log_warn(const std::string& message);

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
// False: each frame is handed over as soon as it is drawn, the CPU waiting for
// the GPU, rather than one frame behind. See melonds.ini in libretro.cpp.
void gfx_set_pipelined(bool pipelined);
// Waits for the GPU to finish everything asked of it so far -- before a
// frame is emulated, since the renderers reuse memory the frame before may
// still be drawing from. See the definition.
void gfx_wait_for_gpu();
// Diagnostics, from /switch/dualscreen/melonds.ini -- see libretro.cpp.
// texcache=0: the 3D renderer's texture cache thrown away before every frame.
void gfx_set_texture_cache(bool keep);
// vramcheck=1: between frames, the video memory the renderers draw from is
// checked against the DS's, and any difference logged.
void gfx_set_vram_check(bool check);
// Told of each buffer upload a renderer records (upload_buffer.cpp): where on
// the GPU it goes, the bytes it was asked to send, and the bytes in the
// staging memory the GPU will copy them from. For vramcheck; none otherwise.
// `sequence` numbers each upload; when g_upload_markers is set, the GPU writes
// it to g_upload_markers[sequence % g_upload_marker_count] (CPU-readable) as
// it reaches that upload's commands, so vramcheck can tell an upload the GPU
// never got to from one it did.
using UploadObserver = void (*)(uint64_t destination, const uint8_t* data, uint32_t size,
                                const uint8_t* staged, uint32_t sequence);
extern UploadObserver g_upload_observer;
extern uint64_t g_upload_markers_gpu;
extern const volatile uint32_t* g_upload_markers;
extern uint32_t g_upload_marker_count;
// Called after each upload's copy is recorded, to record more after it in the
// same command stream: vramcheck's note, over each block the upload covered,
// of which upload the GPU ran last for it.
using UploadWitness = void (*)(DkCmdBuf cmdbuf, uint64_t destination, uint32_t size, uint32_t sequence);
extern UploadWitness g_upload_witness;
// renderer=soft: melonDS's own software renderers draw both the 2D and the 3D,
// on the CPU, in place of the deko3d ones -- for finding out whether wrong
// graphics come from drawing on the GPU at all. gfx_read_screens() then takes
// the frame from melonDS's framebuffers. Before the game loads.
void gfx_set_software(bool software);
bool gfx_software();
// shaders=<.nro>: the renderers' shaders read from that .nro's romfs -- the
// standalone melonDS release's, compiled by its own build -- rather than this
// build's. Before gfx_init()'s renderers load them; false, with why, when the
// .nro has none.
bool gfx_use_shaders_from(const std::string& nro_path, std::string* why);

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
