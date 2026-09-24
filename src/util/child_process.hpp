#pragma once

#include <string>
#include <vector>

namespace ds {

// A program this app started and is responsible for stopping again.
//
// It exists for the console that is played by an emulator running beside this
// app: in Game Mode there is no desktop to go and start one from, so the shell
// has to. See docs/cemu-integration.md.
//
// Stopping it is the point as much as starting it. A shell that quit and left
// an emulator running would leave the player looking at a window they cannot
// reach, on a machine whose only interface is the one that just exited.
class ChildProcess {
public:
    ~ChildProcess();

    // Runs `program` with `arguments`, neither of which is passed through a
    // shell: there is no quoting to get wrong and nothing in a filename can
    // become a command.
    // `environment` is set in its environment on top of this one's, each
    // entry "NAME=value".
    bool start(const std::string& program, const std::vector<std::string>& arguments,
               std::string* error, const std::vector<std::string>& environment = {});
    // True while it is still running. Reaps it if it has already exited, so a
    // program that stops on its own does not stay a zombie for the session.
    bool running();
    // Asks it to stop, and insists if it will not. Returns once it is gone.
    void stop();

private:
    int pid_ = -1;
};

}  // namespace ds
