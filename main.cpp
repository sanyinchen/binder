/*
 * Demo entry point.
 *
 * Orchestrates the whole binder demo in one process tree:
 *
 *   servicemanager   the context manager, binder handle 0
 *        |
 *   demo_service     registers "demo.service" with it
 *        |
 *   demo_client      looks the service up and exercises it
 *
 * Children inherit stdout/stderr, so service-side and client-side output
 * interleave live -- which is the interesting part of watching IPC happen.
 *
 * Usage:
 *   binder_demo              run servicemanager + service + client (default)
 *   binder_demo service      run only the demo service (foreground)
 *   binder_demo client       run only the demo client (foreground)
 *   binder_demo servicemanager
 *
 * The binder driver must already be set up. Inside the container the entrypoint
 * does that; otherwise see README.md.
 */

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

pid_t gServiceManagerPid = -1;
pid_t gServicePid = -1;

bool isExecutable(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string selfDir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    const char* slash = strrchr(buf, '/');
    return slash ? std::string(buf, slash - buf) : std::string(".");
}

// The binaries sit next to us in an install tree, or under examples/ in a
// build tree. Try both before giving up.
std::string findBinary(const std::string& name) {
    const std::string dir = selfDir();
    const std::vector<std::string> candidates = {
            dir + "/" + name,
            dir + "/examples/" + name,
            dir + "/../bin/" + name,
            "/usr/local/bin/" + name,
    };
    for (const auto& path : candidates) {
        if (isExecutable(path)) return path;
    }
    return "";
}

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

// Returns false if the child is already gone, i.e. it failed at startup.
bool stillAlive(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == 0;
}

void reap(pid_t& pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    pid = -1;
}

void cleanup() {
    reap(gServicePid);
    reap(gServiceManagerPid);
}

void onSignal(int sig) {
    cleanup();
    _exit(128 + sig);
}

void banner(const char* text) {
    printf("\n\033[1;36m===== %s =====\033[0m\n", text);
    fflush(stdout);
}

int runDemo() {
    if (access("/dev/binder", F_OK) != 0) {
        fprintf(stderr, "[demo] /dev/binder is missing -- the binder driver is not set up.\n");
        fprintf(stderr, "[demo] Run this through scripts/run.sh, or see README.md.\n");
        return 1;
    }

    const std::string smPath = findBinary("servicemanager");
    const std::string svcPath = findBinary("demo_service");
    const std::string cliPath = findBinary("demo_client");
    const std::vector<std::pair<const char*, const std::string*>> required = {
            {"servicemanager", &smPath},
            {"demo_service", &svcPath},
            {"demo_client", &cliPath},
    };
    for (const auto& [name, path] : required) {
        if (path->empty()) {
            fprintf(stderr, "[demo] could not find '%s' -- has the project been built?\n", name);
            return 1;
        }
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    banner("starting servicemanager");
    gServiceManagerPid = spawn(smPath);
    if (gServiceManagerPid < 0) return 1;
    usleep(500 * 1000);
    if (!stillAlive(gServiceManagerPid)) {
        fprintf(stderr, "[demo] servicemanager exited immediately\n");
        gServiceManagerPid = -1;
        return 1;
    }
    printf("[demo] servicemanager running (pid %d)\n", gServiceManagerPid);

    banner("starting demo service");
    gServicePid = spawn(svcPath);
    if (gServicePid < 0) {
        cleanup();
        return 1;
    }
    usleep(500 * 1000);
    if (!stillAlive(gServicePid)) {
        fprintf(stderr, "[demo] demo_service exited immediately\n");
        gServicePid = -1;
        cleanup();
        return 1;
    }
    printf("[demo] demo service running (pid %d)\n", gServicePid);

    banner("running demo client");
    const pid_t clientPid = spawn(cliPath);
    if (clientPid < 0) {
        cleanup();
        return 1;
    }

    int status = 0;
    waitpid(clientPid, &status, 0);
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    cleanup();

    printf("\n");
    if (rc == 0) {
        printf("\033[1;32m[demo] SUCCESS\033[0m\n");
    } else {
        printf("\033[1;31m[demo] FAILED (client exit %d)\033[0m\n", rc);
    }
    fflush(stdout);
    return rc;
}

int runOne(const char* name) {
    const std::string path = findBinary(name);
    if (path.empty()) {
        fprintf(stderr, "[demo] could not find '%s' -- has the project been built?\n", name);
        return 1;
    }
    execl(path.c_str(), path.c_str(), nullptr);
    fprintf(stderr, "[demo] exec %s failed: %s\n", path.c_str(), strerror(errno));
    return 127;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "demo";

    if (mode == "demo" || mode == "all") return runDemo();
    if (mode == "service") return runOne("demo_service");
    if (mode == "client") return runOne("demo_client");
    if (mode == "servicemanager") return runOne("servicemanager");

    fprintf(stderr, "usage: %s [demo|service|client|servicemanager]\n", argv[0]);
    return 2;
}
