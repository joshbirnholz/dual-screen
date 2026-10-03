// SPDX-License-Identifier: GPL-3.0-or-later
//
// melonDS's frontend/switch/UploadBuffer.cpp, which the renderers stage every
// upload through -- the same, line for line, but for telling vramcheck (see
// gfx.cpp) about each upload as it is recorded: where it goes, the bytes it
// was asked to send, and the bytes now sitting in the staging memory the GPU
// copies from -- and for letting it record its note after each copy. Built
// in its place; see Makefile.

#include "frontend/switch/UploadBuffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "frontend/switch/Gfx.h"
#include "melonds_deko.hpp"

namespace melonds_deko {
UploadObserver g_upload_observer = nullptr;
UploadWitness g_upload_witness = nullptr;
uint64_t g_upload_markers_gpu = 0;
const volatile uint32_t* g_upload_markers = nullptr;
uint32_t g_upload_marker_count = 0;
static uint32_t g_upload_sequence = 0;
}  // namespace melonds_deko

UploadBuffer::UploadBuffer()
{
    for (u32 i = 0; i < 3; i++)
        Buffers[i] = Gfx::DataHeap->Alloc(SegmentSize, 64);
}

DkGpuAddr UploadBuffer::UploadData(dk::CmdBuf cmdbuf, u32 size, u8* data)
{
    if (CurOffset + size > SegmentSize)
    {
        cmdbuf.signalFence(Fences[CurBuffer]);
        CurOffset = 0;
        CurBuffer++;
        if (CurBuffer == 3) CurBuffer = 0;
        LastFlushBuffer++;
        assert(LastFlushBuffer <= 3);
        Fences[CurBuffer].wait();
    }

    u8* srcCpu = Gfx::DataHeap->CpuAddr<u8>(Buffers[CurBuffer]) + CurOffset;
    DkGpuAddr srcGpu = Gfx::DataHeap->GpuAddr(Buffers[CurBuffer]) + CurOffset;
    memcpy(srcCpu, data, size);

    CurOffset += size + 63;
    CurOffset &= ~63;
    return srcGpu;
}

void UploadBuffer::UploadAndCopyData(dk::CmdBuf cmdbuf, DkGpuAddr dst, u8* data, u32 size)
{
    const DkGpuAddr src = UploadData(cmdbuf, size, data);
    const u32 sequence = ++melonds_deko::g_upload_sequence;
    if (melonds_deko::g_upload_observer != nullptr)
    {
        // Where UploadData() just put it, for the CPU.
        const u8* staged = Gfx::DataHeap->CpuAddr<u8>(Buffers[CurBuffer]) +
                           (src - Gfx::DataHeap->GpuAddr(Buffers[CurBuffer]));
        melonds_deko::g_upload_observer(dst, data, size, staged, sequence);
    }
    cmdbuf.copyBuffer(src, dst, size);
    if (melonds_deko::g_upload_marker_count != 0)
    {
        // The GPU says it got this far, in the same stream as the copy.
        cmdbuf.pushData(melonds_deko::g_upload_markers_gpu +
                            (sequence % melonds_deko::g_upload_marker_count) * sizeof(u32),
                        &sequence, sizeof(sequence));
    }
    if (melonds_deko::g_upload_witness != nullptr)
        melonds_deko::g_upload_witness(cmdbuf, dst, size, sequence);
}

void UploadBuffer::UploadAndCopyTexture(dk::CmdBuf cmdbuf, dk::Image& image,
    u8* data,
    u32 x, u32 y, u32 width, u32 height, u32 pitch, u32 layer)
{
    DkCopyBuf src {UploadData(cmdbuf, pitch*height, data)};
    dk::ImageView dst{image};
    cmdbuf.copyBufferToImage(src, dst, {0, 0, layer, width, height, 1});
}
