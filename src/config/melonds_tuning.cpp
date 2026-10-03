#include "config/melonds_tuning.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "config/paths.hpp"

namespace ds {
namespace {

// A line's key and value as the core reads them: spaces anywhere ignored,
// comments starting with # or ;. False for a line that is not a setting.
bool parse_line(const std::string& line, std::string& key, std::string& value) {
    std::string text;
    for (const char c : line) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') text += c;
    }
    if (text.empty() || text[0] == '#' || text[0] == ';') return false;
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos) return false;
    key = text.substr(0, equals);
    value = text.substr(equals + 1);
    return true;
}

// The settings this writes; every other line is left as it is.
constexpr const char* kKeys[] = {"cpu_mhz", "pipeline", "gpu_alone"};

// Settings tried against the graphics glitches and taken out again, which a
// file from then may still hold: dropped when the file is written.
constexpr const char* kRetiredKeys[] = {"gpucache", "l2flush", "settle"};

bool one_of(const std::string& key, const char* const (&keys)[3]) {
    return std::any_of(std::begin(keys), std::end(keys), [&](const char* known) { return key == known; });
}

// A setting's line, or empty when it is at its default and needs none.
std::string line_for(const std::string& key, const MelonDsTuning& tuning) {
    const MelonDsTuning defaults;
    const auto flag = [&](bool value, bool fallback) {
        return value == fallback ? std::string() : key + "=" + (value ? "1" : "0");
    };
    if (key == "cpu_mhz") {
        return tuning.cpu_mhz == defaults.cpu_mhz ? std::string() : key + "=" + std::to_string(tuning.cpu_mhz);
    }
    if (key == "pipeline") return flag(tuning.pipeline, defaults.pipeline);
    if (key == "gpu_alone") return flag(tuning.gpu_alone, defaults.gpu_alone);
    return std::string();
}

}  // namespace

bool MelonDsTuning::operator==(const MelonDsTuning& other) const {
    return cpu_mhz == other.cpu_mhz && pipeline == other.pipeline && gpu_alone == other.gpu_alone;
}

MelonDsTuning read_melonds_tuning(const std::string& path) {
    MelonDsTuning tuning;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::string key;
        std::string value;
        if (!parse_line(line, key, value)) continue;
        const bool on = value != "0";
        if (key == "cpu_mhz") {
            const int mhz = std::atoi(value.c_str());
            if (std::find(std::begin(kMelonDsCpuMhz), std::end(kMelonDsCpuMhz), mhz) != std::end(kMelonDsCpuMhz)) {
                tuning.cpu_mhz = mhz;
            }
        } else if (key == "pipeline") {
            tuning.pipeline = on;
        } else if (key == "gpu_alone") {
            tuning.gpu_alone = on;
        }
    }
    return tuning;
}

bool write_melonds_tuning(const MelonDsTuning& tuning, const std::string& path, std::string* error) {
    std::vector<std::string> lines;
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) lines.push_back(line);
    }
    std::vector<std::string> written;
    std::string out;
    for (const std::string& line : lines) {
        std::string key;
        std::string value;
        const bool setting = parse_line(line, key, value);
        if (setting && one_of(key, kRetiredKeys)) continue;
        if (setting && one_of(key, kKeys)) {
            // A setting's own line, the first time: what it says now. A
            // second line for the same setting would only contradict it.
            if (std::find(written.begin(), written.end(), key) != written.end()) continue;
            written.push_back(key);
            const std::string replacement = line_for(key, tuning);
            if (!replacement.empty()) out += replacement + "\n";
            continue;
        }
        out += line + "\n";
    }
    for (const char* key : kKeys) {
        if (std::find(written.begin(), written.end(), key) != written.end()) continue;
        const std::string line = line_for(key, tuning);
        if (!line.empty()) out += line + "\n";
    }
    ensure_directory(path_dirname(path));
    return write_file_atomically(path, reinterpret_cast<const uint8_t*>(out.data()), out.size(), error);
}

int next_melonds_cpu_mhz(int mhz, int direction) {
    constexpr int kCount = static_cast<int>(sizeof(kMelonDsCpuMhz) / sizeof(kMelonDsCpuMhz[0]));
    int index = 0;
    for (int i = 0; i < kCount; ++i) {
        if (kMelonDsCpuMhz[i] == mhz) index = i;
    }
    // Listed fastest first; "right" is faster, as a value going up.
    index = std::clamp(index - (direction > 0 ? 1 : -1), 0, kCount - 1);
    return kMelonDsCpuMhz[index];
}

}  // namespace ds
