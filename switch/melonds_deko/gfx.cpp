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
#include "GPU3D.h"
#include "frontend/switch/Gfx.h"

namespace melonds_deko {
namespace {

// The romfs of another .nro -- melonDS's own release -- when its shaders are
// the ones to use. See gfx_use_shaders_from().
std::vector<uint8_t> g_shader_romfs;

uint32_t le32(const uint8_t* at) { return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24; }
uint64_t le64(const uint8_t* at) { return uint64_t(le32(at)) | uint64_t(le32(at + 4)) << 32; }

// A file in a romfs by its path, "shaders/X.dksh": each directory found among
// its parent's children, then the file among the last one's. The romfs
// layout libnx reads its own with.
bool romfs_lookup(const std::vector<uint8_t>& romfs, const std::string& path, const uint8_t*& data,
                  std::size_t& size) {
    if (romfs.size() < 0x50) return false;
    const uint64_t dir_table = le64(&romfs[0x18]);
    const uint64_t dir_table_size = le64(&romfs[0x20]);
    const uint64_t file_table = le64(&romfs[0x38]);
    const uint64_t file_table_size = le64(&romfs[0x40]);
    const uint64_t file_data = le64(&romfs[0x48]);
    if (dir_table + dir_table_size > romfs.size() || file_table + file_table_size > romfs.size()) return false;
    const auto name_at = [&](uint64_t entry, uint64_t header) {
        const uint32_t length = le32(&romfs[entry + header - 4]);
        if (entry + header + length > romfs.size()) return std::string();
        return std::string(reinterpret_cast<const char*>(&romfs[entry + header]), length);
    };
    uint32_t dir = 0;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (slash == std::string::npos) {
            for (uint32_t file = le32(&romfs[dir_table + dir + 12]); file != 0xFFFFFFFF;
                 file = le32(&romfs[file_table + file + 4])) {
                if (file + 0x20 > file_table_size) return false;
                if (name_at(file_table + file, 0x20) != part) continue;
                const uint64_t offset = file_data + le64(&romfs[file_table + file + 8]);
                size = static_cast<std::size_t>(le64(&romfs[file_table + file + 16]));
                if (offset + size > romfs.size()) return false;
                data = &romfs[offset];
                return true;
            }
            return false;
        }
        bool found = false;
        for (uint32_t child = le32(&romfs[dir_table + dir + 8]); child != 0xFFFFFFFF;
             child = le32(&romfs[dir_table + child + 4])) {
            if (child + 0x18 > dir_table_size) return false;
            if (name_at(dir_table + child, 0x18) == part) {
                dir = child;
                found = true;
                break;
            }
        }
        if (!found) return false;
        start = slash + 1;
    }
}

}  // namespace
}  // namespace melonds_deko

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
    // Every shader whole, from the other .nro's romfs if its are the ones to
    // use and it has this one, else from this program's.
    std::vector<uint8_t> bytes;
    std::string where = path;
    const std::string romfs_prefix = "romfs:/";
    const uint8_t* borrowed = nullptr;
    std::size_t borrowed_size = 0;
    if (!melonds_deko::g_shader_romfs.empty() && where.rfind(romfs_prefix, 0) == 0 &&
        melonds_deko::romfs_lookup(melonds_deko::g_shader_romfs, where.substr(romfs_prefix.size()), borrowed,
                                   borrowed_size)) {
        bytes.assign(borrowed, borrowed + borrowed_size);
        where += " (from melonDS's own release)";
    } else if (std::FILE* file = std::fopen(path, "rb")) {
        std::fseek(file, 0, SEEK_END);
        const long length = std::ftell(file);
        std::rewind(file);
        if (length > 0) {
            bytes.resize(static_cast<std::size_t>(length));
            if (std::fread(bytes.data(), bytes.size(), 1, file) != 1) bytes.clear();
        }
        std::fclose(file);
    }
    if (bytes.empty()) {
        // melonDS's own frontend asserts here. A missing shader is a build
        // that forgot them, and gfx_init() has already checked for that, so
        // this says which one and leaves the shader empty rather than taking
        // the whole program down.
        melonds_deko::log_error(std::string("melonDS (deko3d): no shader at ") + path);
        return;
    }
    DkshHeader header{};
    bool ok = bytes.size() >= sizeof(header);
    if (ok) {
        std::memcpy(&header, bytes.data(), sizeof(header));
        ok = header.control_sz >= sizeof(header) && uint64_t(header.control_sz) + header.code_sz <= bytes.size();
    }
    if (!ok) {
        melonds_deko::log_error("melonDS (deko3d): could not read the shader " + where);
        return;
    }
    const GpuMemHeap::Allocation code = ShaderCodeHeap->Alloc(header.code_sz, DK_SHADER_CODE_ALIGNMENT);
    std::memcpy(ShaderCodeHeap->CpuAddr<void>(code), bytes.data() + header.control_sz, header.code_sz);
    dk::ShaderMaker{ShaderCodeHeap->MemBlock, code.Offset}
        .setControl(bytes.data())
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

// How much the renderers draw from: exactly what melonDS's own Switch
// frontend gives them, 388 MB between the three, so nothing about the memory
// the renderers work in differs from the program they were written for. Each
// renderer keeps three 16 MB upload buffers and the 3D one about 30 MB of
// tiles; textures are the DS's own, cached in arrays of up to 8 MB a size and
// never given back while a game runs, so a game with many of them needs the
// room.
constexpr uint32_t kDataHeapSize = 256u << 20;
constexpr uint32_t kTextureHeapSize = 120u << 20;
constexpr uint32_t kShaderHeapSize = 12u << 20;
// Room for everything else in the program once the heaps are taken: SDL, Mesa
// and the window this program draws its own screens in.
constexpr uint64_t kHeadroom = 96ull << 20;

// The copy that takes each finished frame off the GPU: a queue of its own, so
// nothing is ever recorded into the renderers' command buffer, and a buffer the
// CPU can read, both screens one after the other -- two of them, the frame
// just finished going into one while the one before it is read out of the
// other. See gfx_read_screens().
dk::Queue g_copy_queue;
dk::CmdBuf g_copy_cmd;
dk::MemBlock g_copy_cmd_memory;
dk::MemBlock g_readback;
dk::Fence g_copied[2];
int g_next_slot = 0;
// The slot holding a frame copied but not yet handed over; -1 for none.
int g_pending_slot = -1;
bool g_pipelined = true;
bool g_keep_texture_cache = true;
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
    uint64_t idle_ticks = 0;
    uint64_t worst_idle_ticks = 0;
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
    g_readback = dk::MemBlockMaker{Gfx::Device, align_up(kScreenBytes * 4, DK_MEMBLOCK_ALIGNMENT)}
                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                     .create();

    g_next_slot = 0;
    g_pending_slot = -1;
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

void gfx_reset_readback() { g_pending_slot = -1; }

void gfx_set_texture_cache(bool keep) { g_keep_texture_cache = keep; }

bool gfx_use_shaders_from(const std::string& nro_path, std::string* why) {
    g_shader_romfs.clear();
    const auto fail = [&](const std::string& reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    std::FILE* file = std::fopen(nro_path.c_str(), "rb");
    if (file == nullptr) return fail("there is no " + nro_path);
    uint8_t start[0x80];
    uint8_t assets[0x38];
    bool ok = std::fread(start, sizeof(start), 1, file) == 1 && le32(start + 0x10) == 0x304F524E;  // NRO0
    const uint32_t nro_size = ok ? le32(start + 0x18) : 0;
    ok = ok && std::fseek(file, static_cast<long>(nro_size), SEEK_SET) == 0 &&
         std::fread(assets, sizeof(assets), 1, file) == 1 && le32(assets) == 0x54455341;  // ASET
    const uint64_t romfs_offset = ok ? le64(assets + 0x28) : 0;
    const uint64_t romfs_size = ok ? le64(assets + 0x30) : 0;
    ok = ok && romfs_size > 0x50 && romfs_size < (64ull << 20);
    if (ok) {
        g_shader_romfs.resize(static_cast<std::size_t>(romfs_size));
        ok = std::fseek(file, static_cast<long>(nro_size + romfs_offset), SEEK_SET) == 0 &&
             std::fread(g_shader_romfs.data(), g_shader_romfs.size(), 1, file) == 1;
    }
    std::fclose(file);
    const uint8_t* probe = nullptr;
    std::size_t probe_size = 0;
    if (!ok || !romfs_lookup(g_shader_romfs, "shaders/FullscreenQuad_vsh.dksh", probe, probe_size)) {
        g_shader_romfs.clear();
        return fail(nro_path + " has no melonDS shaders in it");
    }
    return true;
}

void gfx_set_pipelined(bool pipelined) {
    g_pipelined = pipelined;
    g_pending_slot = -1;
}

void gfx_wait_for_gpu() {
    // The renderers assume the GPU has finished with a frame before the next
    // one is emulated. A texture the game changes is dropped from the 3D
    // renderer's cache and its image handed straight to the next texture
    // uploaded, and that upload must not land while the frame before is
    // still drawing with the old one -- which is what the game's graphics
    // coming out with the wrong textures, on this device and the one it
    // streams to alike, turned out to be. melonDS's own frontend never runs
    // two frames without a display refresh between them, which leaves the GPU
    // time to finish; this program can, when it runs a frame or two back to
    // back to catch up, so here it waits. Almost always for nothing: frames
    // are paced, and by the time the next one is due the GPU is long idle.
    const uint64_t start = armGetSystemTick();
    Gfx::EmuQueue.waitIdle();
    const uint64_t idle = armGetSystemTick() - start;
    // texcache=0: every texture decoded and uploaded afresh, each frame. Slow,
    // and only for finding out whether the cache is what goes wrong -- the GPU
    // is idle, so nothing is still reading what this frees.
    if (!g_keep_texture_cache && GPU3D::CurrentRenderer) GPU3D::CurrentRenderer->Reset();
    g_times.idle_ticks += idle;
    g_times.worst_idle_ticks = std::max(g_times.worst_idle_ticks, idle);
}

void gfx_read_screens(uint32_t* out) {
    auto* renderer = static_cast<GPU2D::DekoRenderer*>(GPU::GPU2D_Renderer.get());
    const int front = GPU::FrontBuffer;
    const int slot = g_next_slot;
    g_next_slot ^= 1;

    // After the frame the renderer has just submitted is drawn: both screens
    // out -- image 0 is always the upper one, whichever engine drew it -- then
    // the images back to the renderer, which waits for exactly this before it
    // draws into them again. The same handshake melonDS's own frontend does
    // around putting them on the screen.
    g_copy_cmd.clear();
    g_copy_cmd.waitFence(renderer->FramebufferReady[front]);
    for (uint32_t screen = 0; screen < 2; ++screen) {
        dk::ImageView view{renderer->GetFramebuffer(front, screen)};
        const DkCopyBuf destination{
            g_readback.getGpuAddr() + (slot * 2 + screen) * kScreenBytes, 0, 0};
        g_copy_cmd.copyImageToBuffer(view, {0, 0, 0, kScreenWidth, kScreenHeight, 1}, destination);
    }
    g_copy_cmd.signalFence(renderer->FramebufferPresented[front]);
    g_copy_cmd.signalFence(g_copied[slot]);
    g_copy_queue.submitCommands(g_copy_cmd.finishList());
    g_copy_queue.flush();

    // What is handed over is the frame before this one, whose copy went in a
    // whole frame ago and is long done: the GPU draws this frame while the
    // CPU emulates the next, as melonDS's own frontend has them, rather than
    // the CPU standing idle while it does. Waiting on this frame instead cost
    // 3-4ms of every frame in a Mario Kart race -- the difference between
    // keeping up and not, in its busiest stretches -- for a picture one frame
    // sooner. After a load or a reset there is no frame before, and this
    // one is waited for, once.
    const int deliver = g_pipelined && g_pending_slot >= 0 ? g_pending_slot : slot;
    g_pending_slot = g_pipelined ? slot : -1;
    const uint64_t wait_start = armGetSystemTick();
    g_copied[deliver].wait();
    const uint64_t convert_start = armGetSystemTick();

    // RGBA8 as the renderer draws it, to libretro's XRGB8888. Copied out of
    // the uncached block in one pass first, which is far quicker than reading
    // it a pixel at a time.
    static uint32_t staged[kScreenWidth * kScreenHeight * 2];
    std::memcpy(staged,
                static_cast<const uint8_t*>(g_readback.getCpuAddr()) + deliver * 2 * kScreenBytes,
                sizeof(staged));
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
                 " ms to convert; the GPU was still busy with the frame before for " +
                 ms_text(ticks_ms(g_times.idle_ticks) / frames) + " ms on average (at worst " +
                 ms_text(ticks_ms(g_times.worst_idle_ticks)) + " ms)");
        g_times = {};
    }
}

}  // namespace melonds_deko
