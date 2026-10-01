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
#include "GPU3D.h"
#include "frontend/switch/CmdMemRing.h"
#include "frontend/switch/Gfx.h"
#include "frontend/switch/UploadBuffer.h"
// vramcheck=1 reads where the 2D renderer keeps its copy of video memory on
// the GPU, which it keeps to itself. A diagnostic only; nothing else here
// looks inside it.
#define private public
#include "GPU2D_Deko.h"
#undef private

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
bool g_vram_check = false;
bool g_gpu_cache = true;
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

// vramcheck=1: whether what the renderers draw from is what the DS has.
//
// Between frames, with the GPU idle, three things must agree. The DS's video
// memory; melonDS's flat copy of it (GPU::VRAMFlat_*), which it brings up to
// date from the DS's wherever its dirty tracking says something changed; and
// the 2D renderer's copy of that on the GPU, which it uploads from the flat
// copy as it changes. A frame ends just after the renderer has submitted
// everything it uploaded, so once the GPU is idle all three must match, but
// for blocks the dirty tracking says changed since and are not uploaded yet.
// Anything else is the cause of wrong graphics found: memory written that the
// emulator never wrote.
namespace vramcheck {

constexpr uint32_t kBlock = GPU::VRAMDirtyGranularity;  // 512
constexpr uint32_t kBankMask[9] = {0x1FFFF, 0x1FFFF, 0x1FFFF, 0x1FFFF, 0xFFFF, 0x3FFF, 0x3FFF, 0x7FFF, 0x3FFF};
constexpr uint64_t kEveryFrames = 30;
constexpr uint64_t kWarmupFrames = 120;
constexpr int kMostReports = 40;

dk::MemBlock g_readback;   // the GPU's copies of 2D video memory, read back
dk::MemBlock g_cmd_memory;
dk::CmdBuf g_cmd;
dk::Fence g_done;
uint64_t g_frames = 0;
uint64_t g_checks = 0;
uint64_t g_checks_with_differences = 0;
int g_reports = 0;
bool g_set_up = false;


struct Difference {
    uint32_t blocks = 0;
    uint32_t first = UINT32_MAX;
    uint64_t expected = 0;
    uint64_t found = 0;
    void note(uint32_t offset, uint64_t want, uint64_t got) {
        if (first == UINT32_MAX) {
            first = offset;
            expected = want;
            found = got;
        }
    }
};

std::string hex(uint64_t value, int digits) {
    char text[24];
    std::snprintf(text, sizeof(text), "%0*llx", digits, static_cast<unsigned long long>(value));
    return text;
}

void report(const std::string& what, const Difference& difference, uint32_t blocks) {
    if (difference.blocks == 0 || g_reports >= kMostReports) return;
    ++g_reports;
    log_warn("melonDS (deko3d): vramcheck: " + what + " in " + std::to_string(difference.blocks) + " of " +
             std::to_string(blocks) + " blocks, first at 0x" + hex(difference.first, 5) + " (" +
             hex(difference.expected, 16) + " expected, " + hex(difference.found, 16) + " found)");
}

// What the renderer recorded for upload into each of its four copies of 2D
// video memory, as it recorded it, from upload_buffer.cpp: the bytes it asked
// to send, and which frame it last asked. Against the GPU's copy and the flat
// one, a stale block on the GPU is either never asked for or asked for and
// not there.
struct Shadow {
    DkGpuAddr base = 0;
    uint32_t size = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> asked_frame;  // per block
};
Shadow g_shadows[4];
// Uploads whose staging memory did not hold the bytes just written to it.
uint64_t g_staging_mismatches = 0;

void observe_upload(uint64_t destination, const uint8_t* data, uint32_t size, const uint8_t* staged) {
    for (Shadow& shadow : g_shadows) {
        if (shadow.size == 0 || destination < shadow.base || destination + size > shadow.base + shadow.size) continue;
        const uint32_t offset = static_cast<uint32_t>(destination - shadow.base);
        std::memcpy(shadow.bytes.data() + offset, data, size);
        for (uint32_t block = offset / kBlock; block <= (offset + size - 1) / kBlock; ++block) {
            shadow.asked_frame[block] = g_frames;
        }
        if (std::memcmp(staged, data, size) != 0) {
            if (g_staging_mismatches++ < 5) {
                log_warn("melonDS (deko3d): vramcheck: an upload's staging memory did not hold the bytes just "
                         "written to it (" + std::to_string(size) + " bytes for offset 0x" + hex(offset, 5) + ")");
            }
        }
        return;
    }
}

// The flat copy against the DS's own video memory, wherever the dirty
// tracking says nothing has changed since the flat copy was brought up to
// date: what DeriveState() would find, without using it up.
template <uint32_t Size, uint32_t Granularity>
Difference flat_against_vram(const uint8_t* flat, const uint32_t* maps,
                             const GPU::VRAMTrackingSet<Size, Granularity>& tracking, uint64_t (*read)(uint32_t)) {
    Difference difference;
    const uint32_t per_mapping = Granularity / kBlock;
    for (uint32_t block = 0; block < Size / kBlock; ++block) {
        const uint32_t slot = block / per_mapping;
        if (maps[slot] != tracking.Mapping[slot]) continue;  // remapped since: all of it pending
        bool pending = false;
        for (uint32_t banks = maps[slot]; banks != 0 && !pending;) {
            const uint32_t bank = static_cast<uint32_t>(__builtin_ctz(banks));
            banks &= ~(1u << bank);
            if (bank >= 9) continue;
            uint32_t bit;
            if (Granularity == 16 * 1024) {
                bit = block % 32 + (slot & (kBankMask[bank] >> 14)) * 32;
            } else if (Granularity == 8 * 1024) {
                bit = block % 16 + (slot & (kBankMask[bank] >> 13)) * 16;
            } else {
                bit = block % 256;
            }
            pending = GPU::VRAMDirty[bank][bit];
        }
        if (pending) continue;
        const uint32_t offset = block * kBlock;
        bool same = true;
        for (uint32_t at = 0; at < kBlock; at += 8) {
            uint64_t have;
            std::memcpy(&have, flat + offset + at, sizeof(have));
            const uint64_t want = read(offset + at);
            if (have != want) {
                difference.note(offset + at, want, have);
                same = false;
                break;
            }
        }
        if (!same) ++difference.blocks;
    }
    return difference;
}

// The renderer's copy on the GPU, read back, against the flat copy it was
// uploaded from: every byte.
Difference gpu_against_flat(const uint8_t* gpu, const uint8_t* flat, uint32_t size) {
    Difference difference;
    for (uint32_t offset = 0; offset < size; offset += kBlock) {
        if (std::memcmp(gpu + offset, flat + offset, kBlock) == 0) continue;
        ++difference.blocks;
        for (uint32_t at = 0; at < kBlock; at += 8) {
            uint64_t want;
            uint64_t got;
            std::memcpy(&want, flat + offset + at, sizeof(want));
            std::memcpy(&got, gpu + offset + at, sizeof(got));
            if (want != got) {
                difference.note(offset + at, want, got);
                break;
            }
        }
    }
    return difference;
}

struct Area {
    const char* name;
    int texture;  // the renderer's VRAMTextureMemory index
    const uint8_t* flat;
    uint32_t size;
};

void init() {
    auto* renderer = static_cast<GPU2D::DekoRenderer*>(GPU::GPU2D_Renderer.get());
    const uint32_t sizes[4] = {512 * 1024, 128 * 1024, 256 * 1024, 128 * 1024};
    const int textures[4] = {GPU2D::DekoRenderer::textureVRAM_ABG, GPU2D::DekoRenderer::textureVRAM_BBG,
                             GPU2D::DekoRenderer::textureVRAM_AOBJ, GPU2D::DekoRenderer::textureVRAM_BOBJ};
    for (int i = 0; i < 4; ++i) {
        Shadow& shadow = g_shadows[i];
        shadow.base = renderer != nullptr ? Gfx::TextureHeap->GpuAddr(renderer->VRAMTextureMemory[textures[i]]) : 0;
        shadow.size = renderer != nullptr ? sizes[i] : 0;
        shadow.bytes.assign(shadow.size, 0);
        shadow.asked_frame.assign(shadow.size / kBlock, 0);
    }
    g_staging_mismatches = 0;
    g_upload_observer = observe_upload;
    constexpr uint32_t kReadback = 1024 * 1024;  // ABG 512K, BBG 128K, AOBJ 256K, BOBJ 128K
    g_readback = dk::MemBlockMaker{Gfx::Device, kReadback}
                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                     .create();
    g_cmd_memory = dk::MemBlockMaker{Gfx::Device, DK_MEMBLOCK_ALIGNMENT}
                       .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                       .create();
    g_cmd = dk::CmdBufMaker{Gfx::Device}.create();
    g_cmd.addMemory(g_cmd_memory, 0, DK_MEMBLOCK_ALIGNMENT);
    g_frames = g_checks = g_checks_with_differences = 0;
    g_reports = 0;
    g_set_up = true;
    log_info("melonDS (deko3d): vramcheck is on: video memory is checked every " +
             std::to_string(kEveryFrames) + " frames");
}

void deinit() {
    if (!g_set_up) return;
    g_set_up = false;
    g_upload_observer = nullptr;
    g_cmd.destroy();
    g_cmd_memory.destroy();
    g_readback.destroy();
}

// With the GPU idle.
void run(dk::Queue& queue) {
    if (!g_set_up) init();
    auto* renderer = static_cast<GPU2D::DekoRenderer*>(GPU::GPU2D_Renderer.get());
    if (renderer == nullptr || ++g_frames < kWarmupFrames || g_frames % kEveryFrames != 0) return;
    // A frame ends after line 262, where the renderer has just recorded the
    // sprites' uploads for the next frame's line 0 -- recorded, not sent: it
    // sends everything at line 191. Those are sent now, so that what is on the
    // GPU is everything melonDS has uploaded. The renderer opens its commands
    // again at the next line it draws, as it would after line 191.
    if (renderer->CmdBufOpen) {
        Gfx::EmuQueue.submitCommands(renderer->CmdMem.End(Gfx::EmuCmdBuf));
        Gfx::EmuQueue.flush();
        renderer->UploadBuf.LastFlushBuffer = 0;
        renderer->CmdBufOpen = false;
        Gfx::EmuQueue.waitIdle();
    }
    const Area areas[] = {
        {"upper engine backgrounds", GPU2D::DekoRenderer::textureVRAM_ABG, GPU::VRAMFlat_ABG, 512 * 1024},
        {"lower engine backgrounds", GPU2D::DekoRenderer::textureVRAM_BBG, GPU::VRAMFlat_BBG, 128 * 1024},
        {"upper engine sprites", GPU2D::DekoRenderer::textureVRAM_AOBJ, GPU::VRAMFlat_AOBJ, 256 * 1024},
        {"lower engine sprites", GPU2D::DekoRenderer::textureVRAM_BOBJ, GPU::VRAMFlat_BOBJ, 128 * 1024},
    };
    g_cmd.clear();
    uint32_t offset = 0;
    for (const Area& area : areas) {
        g_cmd.copyBuffer(Gfx::TextureHeap->GpuAddr(renderer->VRAMTextureMemory[area.texture]),
                         g_readback.getGpuAddr() + offset, area.size);
        offset += area.size;
    }
    g_cmd.signalFence(g_done);
    queue.submitCommands(g_cmd.finishList());
    queue.flush();
    g_done.wait();

    bool differs = false;
    offset = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const Area& area = areas[index];
        std::vector<uint8_t> gpu(area.size);
        std::memcpy(gpu.data(), static_cast<const uint8_t*>(g_readback.getCpuAddr()) + offset, area.size);
        const Difference difference = gpu_against_flat(gpu.data(), area.flat, area.size);
        report(std::string("the GPU's copy of the ") + area.name + " differs from what melonDS uploaded",
               difference, area.size / kBlock);
        differs = differs || difference.blocks != 0;
        if (difference.blocks != 0 && g_reports < kMostReports) {
            // Which of the two it is, block by block: the newest bytes never
            // asked for, or asked for and not on the GPU.
            const Shadow& shadow = g_shadows[index];
            uint32_t never_asked = 0;
            uint32_t asked_not_there = 0;
            uint64_t newest_ask = 0;
            for (uint32_t at = 0; at < area.size; at += kBlock) {
                if (std::memcmp(gpu.data() + at, area.flat + at, kBlock) == 0) continue;
                if (std::memcmp(shadow.bytes.data() + at, area.flat + at, kBlock) == 0) {
                    ++asked_not_there;
                } else {
                    ++never_asked;
                }
                newest_ask = std::max(newest_ask, shadow.asked_frame[at / kBlock]);
            }
            ++g_reports;
            log_warn("melonDS (deko3d): vramcheck: of those, " + std::to_string(asked_not_there) +
                     " were uploaded with these bytes and are not on the GPU, and " + std::to_string(never_asked) +
                     " were never uploaded with them; the newest upload to any of them was " +
                     (newest_ask == 0 ? std::string("never")
                                      : std::to_string(g_frames - newest_ask) + " frames ago") +
                     "; staging memory mismatches so far: " + std::to_string(g_staging_mismatches));
        }
        offset += area.size;
    }

    const auto flat = [&](const char* name, const Difference& difference, uint32_t size) {
        report(std::string("melonDS's flat copy of the ") + name +
                   " differs from the DS's video memory, with nothing pending",
               difference, size / kBlock);
        differs = differs || difference.blocks != 0;
    };
    flat("upper engine backgrounds",
         flat_against_vram(GPU::VRAMFlat_ABG, GPU::VRAMMap_ABG, GPU::VRAMDirty_ABG, GPU::ReadVRAM_ABG<u64>),
         512 * 1024);
    flat("lower engine backgrounds",
         flat_against_vram(GPU::VRAMFlat_BBG, GPU::VRAMMap_BBG, GPU::VRAMDirty_BBG, GPU::ReadVRAM_BBG<u64>),
         128 * 1024);
    flat("upper engine sprites",
         flat_against_vram(GPU::VRAMFlat_AOBJ, GPU::VRAMMap_AOBJ, GPU::VRAMDirty_AOBJ, GPU::ReadVRAM_AOBJ<u64>),
         256 * 1024);
    flat("lower engine sprites",
         flat_against_vram(GPU::VRAMFlat_BOBJ, GPU::VRAMMap_BOBJ, GPU::VRAMDirty_BOBJ, GPU::ReadVRAM_BOBJ<u64>),
         128 * 1024);
    flat("3D textures",
         flat_against_vram(GPU::VRAMFlat_Texture, GPU::VRAMMap_Texture, GPU::VRAMDirty_Texture,
                           GPU::ReadVRAM_Texture<u64>),
         512 * 1024);
    flat("3D texture palettes",
         flat_against_vram(GPU::VRAMFlat_TexPal, GPU::VRAMMap_TexPal, GPU::VRAMDirty_TexPal,
                           GPU::ReadVRAM_TexPal<u64>),
         128 * 1024);

    ++g_checks;
    if (differs) ++g_checks_with_differences;
    if (g_checks % 20 == 0) {
        log_info("melonDS (deko3d): vramcheck: " + std::to_string(g_checks) + " checks, " +
                 std::to_string(g_checks_with_differences) + " of them with differences");
    }
}

}  // namespace vramcheck

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
    // gpucache=0: what the renderers write -- textures, video memory, the
    // pictures they draw -- kept out of the GPU's L2 cache, and what they
    // stage it from with it. The GPU's L2 is shared with Mesa, which draws this
    // app's own screens; written data waiting in it can be lost to an
    // invalidation from there, which melonDS's own app, the GPU's only user,
    // never meets. Slower, if that is what is wrong.
    const uint32_t gpu_access = g_gpu_cache ? DkMemBlockFlags_GpuCached : DkMemBlockFlags_GpuUncached;
    if (!g_gpu_cache) log_info("melonDS (deko3d): the renderers' GPU memory bypasses the GPU's L2 cache");
    Gfx::TextureHeap.emplace(Gfx::Device, kTextureHeapSize, gpu_access | DkMemBlockFlags_Image, 1024 * 8);
    Gfx::ShaderCodeHeap.emplace(Gfx::Device, kShaderHeapSize,
                                DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                    DkMemBlockFlags_Code,
                                64);
    Gfx::DataHeap.emplace(Gfx::Device, kDataHeapSize, DkMemBlockFlags_CpuUncached | gpu_access, 128);
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
    vramcheck::deinit();
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

void gfx_set_vram_check(bool check) { g_vram_check = check; }

void gfx_set_gpu_cache(bool cached) { g_gpu_cache = cached; }

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
    if (g_vram_check) vramcheck::run(g_copy_queue);
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
    // The commands of the copy still being held are in the memory about to
    // be written over, and with frames handed over one behind it may not have
    // run yet: it is waited for first, which costs nothing, as it is the one
    // this frame hands over anyway.
    if (g_pending_slot >= 0) g_copied[g_pending_slot].wait();
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
