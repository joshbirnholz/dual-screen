#pragma once

#include <string>

namespace ds::gl_trace {

// Instrumented OpenGL entry points handed to a hardware-rendering core.
//
// The frontend supplies the core's GL functions through get_proc_address, which
// means it can supply wrapped ones. Each wrapper calls the real function, then
// asks OpenGL whether that call was rejected -- so "the core left a
// GL_INVALID_VALUE behind somewhere" becomes the exact call and its arguments.
//
// Only worth having while a core is drawing nothing and not saying why, so it
// is off unless DUALSCREEN_TRACE_CORE_GL is set. It calls glGetError after
// every wrapped call, which serialises the driver and is far too slow to leave
// on.
bool enabled();

// The wrapper for `name`, or nullptr if this symbol is not traced -- in which
// case the caller should hand over the driver's own function.
void* wrapper_for(const std::string& name);

// Called once per frame; tracing stops itself after a few frames, since a
// failure that happens at all happens immediately.
void begin_frame();

}  // namespace ds::gl_trace
