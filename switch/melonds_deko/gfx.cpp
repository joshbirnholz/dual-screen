// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GPU side: what RSDuck's frontend/switch/Gfx.cpp sets up for the melonDS
// renderers to draw with, and nothing of what it does to put a picture on the
// screen -- this program already has a window, drawn by SDL through Mesa, and
// a second owner for the console's screen is the one thing that cannot share.
// The renderers use exactly the device, the queue they submit to, three
// memory heaps and LoadShader(); see melonds_deko.hpp for the rest.

#include "melonds_deko.hpp"

#include <malloc.h>
#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "GPU.h"
#include "GPU2D_Deko.h"
#include "frontend/switch/Gfx.h"

namespace Gfx {

// Declared by the fork's Gfx.h, and all that the renderers and the helpers
// compiled with them reach for.
dk::Device Device;
dk::Queue PresentQueue, EmuQueue;
dk::CmdBuf PresentCmdBuf, EmuCmdBuf;
std::optional<GpuMemHeap> TextureHeap;
std::optional<GpuMemHeap> ShaderCodeHeap;
std::optional<GpuMemHeap> DataHeap;

namespace {

// The layout of a .dksh file as uam writes it: a header, then the control
// section (header included) of control_sz bytes, then code_sz bytes of code.
struct DkshHeader {
    uint32_t magic;
    uint32_t header_sz;
    uint32_t control_sz;
    uint32_t code_sz;
    uint32_t programs_off;
    uint32_t num_programs;
};

}  // namespace

void LoadShader(const char* path, dk::Shader& out) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        // melonDS's own frontend asserts here. A missing shader is a build
        // that forgot them, and gfx_init() has already checked for that, so
        // this says which one and leaves the shader empty rather than taking
        // the whole program down.
        melonds_deko::log_error(std::string("melonDS (deko3d): no shader at ") + path);
        return;
    }
    DkshHeader header{};
    std::vector<uint8_t> control;
    bool ok = std::fread(&header, sizeof(header), 1, file) == 1 && header.control_sz >= sizeof(header);
    if (ok) {
        control.resize(header.control_sz);
        std::rewind(file);
        ok = std::fread(control.data(), control.size(), 1, file) == 1;
    }
    GpuMemHeap::Allocation code{};
    if (ok) {
        code = ShaderCodeHeap->Alloc(header.code_sz, DK_SHADER_CODE_ALIGNMENT);
        ok = std::fread(ShaderCodeHeap->CpuAddr<void>(code), header.code_sz, 1, file) == 1;
    }
    std::fclose(file);
    if (!ok) {
        melonds_deko::log_error(std::string("melonDS (deko3d): could not read the shader ") + path);
        return;
    }
    dk::ShaderMaker{ShaderCodeHeap->MemBlock, code.Offset}
        .setControl(control.data())
        .setProgramId(0)
        .initialize(out);
}

}  // namespace Gfx

// The end of the region libnx gave the heap at start-up (newlib's sbrk grows
// towards it).
extern "C" char* fake_heap_end;

namespace melonds_deko {

namespace {

constexpr uint32_t kScreenWidth = 256;
constexpr uint32_t kScreenHeight = 192;
constexpr uint32_t kScreenBytes = kScreenWidth * kScreenHeight * 4;

constexpr uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// How much the renderers draw from. Each renderer keeps three 16 MB upload
// buffers and the 3D one about 30 MB of tiles, which is what sets the data
// heap; textures are the DS's own, cached in arrays of up to 8 MB a size.
// melonDS's frontend asks for 388 MB between them, most of it for its own
// menus, which this program does not have.
constexpr uint32_t kDataHeapSize = 160u << 20;
constexpr uint32_t kTextureHeapSize = 96u << 20;
constexpr uint32_t kShaderHeapSize = 4u << 20;
// Room for everything else in the program once the heaps are taken: SDL, Mesa
// and the window this program draws its own screens in.
constexpr uint64_t kHeadroom = 96ull << 20;

// The copy that takes each finished frame off the GPU: a queue of its own, so
// nothing is ever recorded into the renderers' command buffer, and a buffer the
// CPU can read, both screens one after the other.
dk::Queue g_copy_queue;
dk::CmdBuf g_copy_cmd;
dk::MemBlock g_copy_cmd_memory;
dk::MemBlock g_readback;
dk::Fence g_copied;
bool g_ready = false;

// How long each frame waits for the GPU to finish drawing it and copying it
// out, and how long turning it into libretro's format takes, told every
// ten seconds of frames: the part of a frame's time that is the GPU's rather
// than the emulated CPUs'.
struct ReadbackTimes {
    uint64_t frames = 0;
    uint64_t wait_ticks = 0;
    uint64_t worst_wait_ticks = 0;
    uint64_t convert_ticks = 0;
} g_times;
constexpr uint64_t kTimesEvery = 600;

double ticks_ms(uint64_t ticks) { return double(armTicksToNs(ticks)) / 1e6; }

std::string ms_text(double ms) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", ms);
    return text;
}

void debug_output(void*, const char* context, DkResult result, const char* message) {
    log_error(std::string("deko3d: ") + (context != nullptr ? context : "") + ": " +
              (message != nullptr ? message : "") + " (" + std::to_string(result) + ")");
}

// What malloc can still hand out, which is where deko3d's memory comes from.
// Not the process's free memory: libnx gives nearly all of that to the heap
// at start-up, so by now the system counts it as used. The heap is what is
// left between the break and the end of that region, and whatever malloc has
// taken and freed again.
uint64_t memory_available() {
    char* const brk = static_cast<char*>(sbrk(0));
    const uint64_t unclaimed = (fake_heap_end != nullptr && brk != reinterpret_cast<char*>(-1) &&
                                fake_heap_end > brk)
                                   ? static_cast<uint64_t>(fake_heap_end - brk)
                                   : 0;
    return unclaimed + static_cast<uint64_t>(mallinfo().fordblks);
}

}  // namespace

bool gfx_init(std::string* error) {
    if (g_ready) return true;

    // The shaders are built into the program's romfs (see Makefile). Checked
    // before anything is allocated, since without them there is nothing the
    // renderers can do.
    romfsInit();  // already mounted is fine: the result is not what matters
    if (std::FILE* probe = std::fopen("romfs:/shaders/FullscreenQuad_vsh.dksh", "rb")) {
        std::fclose(probe);
    } else {
        if (error != nullptr) {
            *error = "this build has no deko3d shaders in its romfs -- build the core with "
                     "switch/melonds_deko/Makefile and link with the romfs it makes";
        }
        return false;
    }

    const uint64_t needed =
        uint64_t(kDataHeapSize) + kTextureHeapSize + kShaderHeapSize + kHeadroom;
    const uint64_t available = memory_available();
    log_info("melonDS (deko3d): " + std::to_string(available >> 20) + " MB free, " +
             std::to_string(needed >> 20) + " MB wanted for the GPU and around it");
    if (available < needed) {
        if (error != nullptr) {
            *error = "not enough memory for the GPU renderer (" + std::to_string(available >> 20) +
                     " MB free, " + std::to_string(needed >> 20) +
                     " MB needed) -- start Dualscreen through a game (hold R while starting "
                     "one) rather than from the album, which gives a program far less";
        }
        return false;
    }

    Gfx::Device = dk::DeviceMaker{}.setCbDebug(debug_output).create();
    Gfx::EmuQueue = dk::QueueMaker{Gfx::Device}
                        .setFlags(DkQueueFlags_HighPrio | DkQueueFlags_Graphics |
                                  DkQueueFlags_Compute | DkQueueFlags_DisableZcull)
                        .setCommandMemorySize(DK_QUEUE_MIN_CMDMEM_SIZE * 4)
                        .setFlushThreshold(DK_QUEUE_MIN_CMDMEM_SIZE)
                        .create();
    Gfx::TextureHeap.emplace(Gfx::Device, kTextureHeapSize,
                             DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 1024 * 8);
    Gfx::ShaderCodeHeap.emplace(Gfx::Device, kShaderHeapSize,
                                DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                    DkMemBlockFlags_Code,
                                64);
    Gfx::DataHeap.emplace(Gfx::Device, kDataHeapSize,
                          DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 128);
    Gfx::EmuCmdBuf = dk::CmdBufMaker{Gfx::Device}.create();

    g_copy_queue = dk::QueueMaker{Gfx::Device}.setFlags(DkQueueFlags_Graphics).create();
    constexpr uint32_t kCopyCmdSize = 0x10000;
    g_copy_cmd_memory = dk::MemBlockMaker{Gfx::Device, kCopyCmdSize}
                            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                            .create();
    g_copy_cmd = dk::CmdBufMaker{Gfx::Device}.create();
    g_copy_cmd.addMemory(g_copy_cmd_memory, 0, kCopyCmdSize);
    g_readback = dk::MemBlockMaker{Gfx::Device, align_up(kScreenBytes * 2, DK_MEMBLOCK_ALIGNMENT)}
                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                     .create();

    g_ready = true;
    log_info("melonDS (deko3d): the GPU is set up");
    return true;
}

void gfx_deinit() {
    if (!g_ready) return;
    g_copy_queue.waitIdle();
    Gfx::EmuQueue.waitIdle();
    g_copy_queue.destroy();
    g_copy_cmd.destroy();
    g_copy_cmd_memory.destroy();
    g_readback.destroy();
    Gfx::EmuCmdBuf.destroy();
    Gfx::EmuQueue.destroy();
    Gfx::DataHeap.reset();
    Gfx::ShaderCodeHeap.reset();
    Gfx::TextureHeap.reset();
    Gfx::Device.destroy();
    g_ready = false;
}

void gfx_read_screens(uint32_t* out) {
    auto* renderer = static_cast<GPU2D::DekoRenderer*>(GPU::GPU2D_Renderer.get());
    const int front = GPU::FrontBuffer;

    // After the frame the renderer has just submitted is drawn: both screens
    // out -- image 0 is always the upper one, whichever engine drew it -- then
    // the images back to the renderer, which waits for exactly this before it
    // draws into them again. The same handshake melonDS's own frontend does
    // around putting them on the screen.
    g_copy_cmd.clear();
    g_copy_cmd.waitFence(renderer->FramebufferReady[front]);
    for (uint32_t screen = 0; screen < 2; ++screen) {
        dk::ImageView view{renderer->GetFramebuffer(front, screen)};
        const DkCopyBuf destination{g_readback.getGpuAddr() + screen * kScreenBytes, 0, 0};
        g_copy_cmd.copyImageToBuffer(view, {0, 0, 0, kScreenWidth, kScreenHeight, 1}, destination);
    }
    g_copy_cmd.signalFence(renderer->FramebufferPresented[front]);
    g_copy_cmd.signalFence(g_copied);
    g_copy_queue.submitCommands(g_copy_cmd.finishList());
    g_copy_queue.flush();
    const uint64_t wait_start = armGetSystemTick();
    g_copied.wait();
    const uint64_t convert_start = armGetSystemTick();

    // RGBA8 as the renderer draws it, to libretro's XRGB8888. Copied out of
    // the uncached block in one pass first, which is far quicker than reading
    // it a pixel at a time.
    static uint32_t staged[kScreenWidth * kScreenHeight * 2];
    std::memcpy(staged, g_readback.getCpuAddr(), sizeof(staged));
    for (uint32_t i = 0; i < kScreenWidth * kScreenHeight * 2; ++i) {
        const uint32_t rgba = staged[i];
        out[i] = ((rgba & 0xFFu) << 16) | (rgba & 0xFF00u) | ((rgba >> 16) & 0xFFu);
    }

    const uint64_t wait = convert_start - wait_start;
    g_times.wait_ticks += wait;
    g_times.worst_wait_ticks = std::max(g_times.worst_wait_ticks, wait);
    g_times.convert_ticks += armGetSystemTick() - convert_start;
    if (++g_times.frames == kTimesEvery) {
        const double frames = double(g_times.frames);
        log_info("melonDS (deko3d): each frame waited " + ms_text(ticks_ms(g_times.wait_ticks) / frames) +
                 " ms for the GPU (at worst " + ms_text(ticks_ms(g_times.worst_wait_ticks)) +
                 " ms) and took " + ms_text(ticks_ms(g_times.convert_ticks) / frames) +
                 " ms to convert");
        g_times = {};
    }
}

}  // namespace melonds_deko
