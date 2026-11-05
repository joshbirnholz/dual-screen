#pragma once

// Whether an H.264 encoder will actually open on this machine, not only
// whether FFmpeg has one built in. They differ on Windows run under Wine,
// which has no Media Foundation encoder and no GPU driver: FFmpeg has all of
// Windows's encoders and none of them opens. A test of the video path skips
// there rather than failing on a machine that cannot make video at all.

#include <string>

#include "video/video_codec.hpp"
#include "video/video_encoder.hpp"

namespace test {

inline bool video_encodes_here() {
    static const bool opens = [] {
        if (!ds::video::have_ffmpeg()) return false;
        ds::video::EncoderConfig config;
        config.width = 64;
        config.height = 64;
        ds::video::VideoEncoder encoder;
        std::string error;
        return encoder.open(config, &error);
    }();
    return opens;
}

}  // namespace test
