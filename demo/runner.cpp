#include "runner.h"

#include "binary_paths.h"
#include "child_process.h"

#include <csignal>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace demo {
namespace {

// The two long-lived children, at file scope so the signal handler can reach
// them: Ctrl+C must not leave a servicemanager behind holding the context
// manager slot.
pid_t gServiceManagerPid = -1;
pid_t gServicePid = -1;

void cleanup() {
    terminate(gServicePid);
    terminate(gServiceManagerPid);
}

void onSignal(int sig) {
    cleanup();
    _exit(128 + sig);
}

void banner(const char* text) {
    printf("\n\033[1;36m===== %s =====\033[0m\n", text);
    fflush(stdout);
}

bool binderIsReady() {
    if (access("/dev/binder", F_OK) == 0) return true;
    fprintf(stderr, "[demo] /dev/binder is missing -- the binder driver is not set up.\n");
    fprintf(stderr, "[demo] Run this through scripts/run.sh, or see README.md.\n");
    return false;
}

// Spawns `path` and gives it a moment to fail. Returns -1 if it did.
pid_t startAndSettle(const char* name, const std::string& path, pid_t& slot) {
    slot = spawn(path);
    if (slot < 0) return -1;

    usleep(500 * 1000);
    if (!stillAlive(slot)) {
        fprintf(stderr, "[demo] %s exited immediately\n", name);
        slot = -1; // already reaped by stillAlive()
        return -1;
    }
    printf("[demo] %s running (pid %d)\n", name, slot);
    return slot;
}

} // namespace

int runAll() {
    if (!binderIsReady()) return 1;

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
    if (startAndSettle("servicemanager", smPath, gServiceManagerPid) < 0) {
        cleanup();
        return 1;
    }

    banner("starting demo service");
    if (startAndSettle("demo service", svcPath, gServicePid) < 0) {
        cleanup();
        return 1;
    }

    banner("running demo client");
    const pid_t clientPid = spawn(cliPath);
    if (clientPid < 0) {
        cleanup();
        return 1;
    }
    const int rc = waitForExit(clientPid);

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
    return execReplace(path);
}

} // namespace demo
