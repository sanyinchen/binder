#include "child_process.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace demo {

pid_t spawn(const std::string& path) {
    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[demo] fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), nullptr);
        fprintf(stderr, "[demo] exec %s failed: %s\n", path.c_str(), strerror(errno));
        _exit(127);
    }
    return pid;
}

bool stillAlive(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == 0;
}

int waitForExit(pid_t pid) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void terminate(pid_t& pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    pid = -1;
}

int execReplace(const std::string& path) {
    execl(path.c_str(), path.c_str(), nullptr);
    fprintf(stderr, "[demo] exec %s failed: %s\n", path.c_str(), strerror(errno));
    return 127;
}

} // namespace demo
