#pragma once

#include "input/ds_input.hpp"

namespace ds {

// The V1 InputSink: latches the most recent DS input state so the libretro
// core's input_state callback can read it during retro_run().
class CoreInputSink final : public InputSink {
public:
    void submit(const DsInputState& state) override;

    const DsInputState& state() const { return state_; }
    // Pointer to the latched state, for the core host to read each frame.
    const DsInputState* state_pointer() const { return &state_; }

private:
    DsInputState state_;
};

}  // namespace ds
