#include "core/core_input_sink.hpp"

namespace ds {

void CoreInputSink::submit(const DsInputState& state) { state_ = state; }

}  // namespace ds
