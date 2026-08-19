#include "app.hpp"

int main(int argc, char** argv) {
    const ds::AppOptions options = ds::parse_command_line(argc, argv);
    ds::App app;
    return app.run(options);
}
