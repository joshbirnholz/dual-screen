#include "util/build_stamp.hpp"

#include "util/log.hpp"

namespace ds {

// DS_BUILD_STAMP is defined on this file alone rather than on the target: it
// carries a timestamp, so it changes at every configure, and defining it any
// wider would recompile everything each time for a string one file reads.
std::string build_stamp() {
#ifdef DS_BUILD_STAMP
    return DS_BUILD_STAMP;
#else
    return std::string();
#endif
}

int build_number() {
#ifdef DS_BUILD_NUMBER
    return DS_BUILD_NUMBER;
#else
    return 0;
#endif
}

void log_build_stamp() {
    const std::string stamp = build_stamp();
    if (stamp.empty()) {
        log_info("this build does not say which build it is");
        return;
    }
    const int number = build_number();
    log_info("this build is " + stamp +
             (number > 0 ? ", nightly build " + std::to_string(number) : std::string()));
}

}  // namespace ds
