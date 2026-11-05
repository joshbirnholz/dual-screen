#include "app.hpp"
#include "launch/launch_host.hpp"
#include "update/desktop_updater.hpp"
#include "util/win_console.hpp"

int main(int argc, char** argv) {
    ds::attach_parent_console();
    const ds::AppOptions options = ds::parse_command_line(argc, argv);
    if (options.service) return ds::run_launch_service();
    ds::update::remove_left_behind();
    int status = 0;
    {
        ds::App app;
        status = app.run(options);
    }
    // An update was installed: this process becomes the new build. Only
    // returns when it was not, or could not be.
    ds::update::restart_if_requested(argv);
    return status;
}
