#pragma once

#include <string>

namespace ds {

// How the Switch's melonDS runs: /switch/dualscreen/melonds.ini, the one place
// these live. The core reads it as a game loads and the app as it starts
// playing one; Settings > melonDS reads and writes it. Each setting is a line
// key=value, and a setting with no line is at its default -- so a file nobody
// has touched is an empty one, and one written by hand keeps its comments and
// any lines this does not know when Settings changes it.
inline constexpr const char* kMelonDsTuningPath = "/switch/dualscreen/melonds.ini";

// The speeds melonDS's own Switch app offers, fastest first; the first is the
// default.
inline constexpr int kMelonDsCpuMhz[] = {1785, 1581, 1224, 1020, 918, 816, 714};

struct MelonDsTuning {
    int cpu_mhz = 1785;     // cpu_mhz
    bool jit = true;        // jit
    bool fastmem = true;    // fastmem: the JIT's fast memory
    bool pipeline = true;   // pipeline: each frame handed over a frame behind
    bool gpu_alone = false; // gpu_alone: the app's drawing finished before each frame
    bool texcache = true;   // texcache: the 3D renderer's texture cache kept
    std::string shaders;    // shaders: empty for this build's, "standalone", or an .nro
    bool vramcheck = false; // vramcheck: video memory checked between frames
    bool gpucache = true;   // gpucache: the renderers' GPU memory in the GPU's L2 cache
    bool l2flush = false;   // l2flush: each upload written out of the L2 cache at once
    int settle = 0;         // settle: after each upload's copy, 0 nothing, 1 a small copy, 2 it read back

    bool operator==(const MelonDsTuning& other) const;
};

// What the file says; the defaults where it says nothing, or is not there.
MelonDsTuning read_melonds_tuning(const std::string& path = kMelonDsTuningPath);

// The file made to say `tuning`: each setting's line rewritten, removed when
// it is back at its default, or added; everything else in the file as it was.
bool write_melonds_tuning(const MelonDsTuning& tuning, const std::string& path = kMelonDsTuningPath,
                          std::string* error = nullptr);

// The next speed in kMelonDsCpuMhz from `mhz`, in `direction`, stopping at
// the ends.
int next_melonds_cpu_mhz(int mhz, int direction);

}  // namespace ds
