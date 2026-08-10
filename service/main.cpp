/*
 * service_manager -- the entry point for every server-side process here.
 *
 * With no arguments it is a launcher: it checks that the binder driver is up,
 * starts servicemanager (unless one is already running), fork/execs itself
 * twice to be the http service and the download service, and then watches over
 * them until Ctrl+C.
 *
 *   servicemanager      the context manager, binder handle 0
 *        |
 *   http.service        provides HTTP requests   (service_manager http)
 *        |
 *   download.service    downloads through it     (service_manager download)
 *
 * The two services get a process each rather than two objects in one process:
 * that is what makes the download -> http call a real cross-process binder
 * transaction instead of a plain function call.
 *
 *   service_manager                 servicemanager + both services (default)
 *   service_manager http            just the http service, in the foreground
 *   service_manager download        just the download service, in the foreground
 *   service_manager servicemanager  just servicemanager, in the foreground
 *
 * The binder driver must already be set up -- scripts/run.sh does that via
 * scripts/setup-binder-host.sh; see README.md.
 */

#include "download/DownloadService.h"
#include "http/HttpService.h"

#include <com/service/download/IDownloadService.h>
#include <com/service/http/IHttpService.h>

#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using android::ProcessState;
using android::sp;
using android::String16;

namespace {

// One spawned child: a pid plus a name to log it under.
struct Child {
    const char* name;
    pid_t pid = -1;
};

// At file scope so the signal handler can reach them: Ctrl+C must not leave a
// servicemanager behind holding the context manager slot, nor two orphaned
// services.
Child gServiceManager{"servicemanager"};
Child gHttp{"http service"};
Child gDownload{"download service"};

// A servicemanager that was already running belongs to someone else and has to
// outlive us.
bool gOwnServiceManager = false;

std::string selfPath() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf, n);
}

bool isExecutable(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

// Looks next to this executable, then in ../bin and /usr/local/bin, so both a
// build tree and a `cmake --install` layout work.
std::string findBinary(const std::string& name) {
    const std::string self = selfPath();
    const size_t slash = self.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : self.substr(0, slash);

    for (const std::string& candidate :
         {dir + "/" + name, dir + "/../bin/" + name, "/usr/local/bin/" + name}) {
        if (!isExecutable(candidate)) continue;
        char* resolved = realpath(candidate.c_str(), nullptr);
        if (resolved == nullptr) return candidate;
        std::string canonical(resolved);
        free(resolved);
        return canonical;
    }
    return {};
}

// fork + exec. Children inherit stdout/stderr on purpose, so the processes'
// output interleaves live in one terminal -- that is the interesting part of
// watching IPC.
pid_t spawn(const std::string& path, const char* arg) {
    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[manager] fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), arg, nullptr);
        fprintf(stderr, "[manager] exec %s failed: %s\n", path.c_str(), strerror(errno));
        _exit(127);
    }
    return pid;
}

// False if the child has already exited. Reaps it in that case, so the caller
// must not wait on it again.
bool stillAlive(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == 0;
}

void terminate(Child& child) {
    if (child.pid <= 0) return;
    kill(child.pid, SIGTERM);
    int status = 0;
    waitpid(child.pid, &status, 0);
    child.pid = -1;
}

void cleanup() {
    terminate(gDownload);
    terminate(gHttp);
    if (gOwnServiceManager) terminate(gServiceManager);
}

void onSignal(int sig) {
    cleanup();
    _exit(128 + sig);
}

bool binderIsReady() {
    if (access("/dev/binder", F_OK) == 0) return true;
    fprintf(stderr, "[manager] /dev/binder is missing -- the binder driver is not set up.\n");
    fprintf(stderr, "[manager] Run this through scripts/run.sh, or see README.md.\n");
    return false;
}

// -------------------------------------------------------------------------
// Bringing up servicemanager
//
// defaultServiceManager() never fails when servicemanager is missing -- it
// retries forever, logging "Waiting 1s on context object". getContextObject()
// is the same lookup without the loop, so it works as a one-shot probe: it
// pings binder handle 0 and returns null when nobody holds it.
// -------------------------------------------------------------------------
bool serviceManagerIsUp() {
    return ProcessState::self()->getContextObject(nullptr) != nullptr;
}

bool ensureServiceManager() {
    if (serviceManagerIsUp()) {
        printf("[manager] servicemanager already running, using it\n");
        fflush(stdout);
        return true;
    }

    const std::string path = findBinary("servicemanager");
    if (path.empty()) {
        fprintf(stderr, "[manager] no servicemanager running, and none found to start\n");
        return false;
    }

    gServiceManager.pid = spawn(path, nullptr);
    if (gServiceManager.pid < 0) return false;
    gOwnServiceManager = true; // we started it, so we take it down with us

    for (int i = 0; i < 20; ++i) { // 5s, well past the ~100ms it usually takes
        usleep(250 * 1000);
        if (!stillAlive(gServiceManager.pid)) {
            fprintf(stderr, "[manager] servicemanager exited immediately\n");
            gServiceManager.pid = -1; // already reaped by stillAlive()
            return false;
        }
        if (serviceManagerIsUp()) {
            printf("[manager] servicemanager running (pid %d)\n", gServiceManager.pid);
            fflush(stdout);
            return true;
        }
    }

    fprintf(stderr, "[manager] servicemanager did not come up within 5s\n");
    return false;
}

// Starts one service role and waits until it is actually registered. The order
// matters: the download service expects to find the http service as it starts.
bool startService(Child& child, const char* role, const String16& registeredName) {
    const std::string self = selfPath();
    if (self.empty()) {
        fprintf(stderr, "[manager] cannot locate my own executable (/proc/self/exe)\n");
        return false;
    }

    child.pid = spawn(self, role);
    if (child.pid < 0) return false;

    auto sm = android::defaultServiceManager();
    for (int i = 0; i < 40; ++i) { // 10s
        usleep(250 * 1000);
        if (!stillAlive(child.pid)) {
            fprintf(stderr, "[manager] %s exited immediately\n", child.name);
            child.pid = -1;
            return false;
        }
        if (sm->checkService(registeredName) != nullptr) {
            printf("[manager] %s ready (pid %d)\n", child.name, child.pid);
            fflush(stdout);
            return true;
        }
    }

    fprintf(stderr, "[manager] %s did not register within 10s\n", child.name);
    return false;
}

int runManager() {
    if (!binderIsReady()) return 1;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    if (!ensureServiceManager()) {
        cleanup();
        return 1;
    }
    // Not interchangeable: the download service looks the http service up as it
    // starts.
    if (!startService(gHttp, "http", com::service::http::IHttpService::SERVICE_NAME()) ||
        !startService(gDownload, "download",
                      com::service::download::IDownloadService::SERVICE_NAME())) {
        cleanup();
        return 1;
    }

    printf("\n[manager] all up. Registered services:\n");
    for (const String16& name : android::defaultServiceManager()->listServices()) {
        printf("[manager]   %s\n", android::String8(name).c_str());
    }
    printf("[manager] the client can go now: download_client <url> <destination>\n");
    printf("[manager] Ctrl+C to stop.\n\n");
    fflush(stdout);

    // Any child exiting means something broke: take the rest down rather than
    // leave half a service tree standing.
    while (true) {
        int status = 0;
        const pid_t pid = wait(&status);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const char* who = pid == gServiceManager.pid ? gServiceManager.name
                          : pid == gHttp.pid         ? gHttp.name
                          : pid == gDownload.pid     ? gDownload.name
                                                     : "child";
        if (pid == gServiceManager.pid) gServiceManager.pid = -1;
        if (pid == gHttp.pid) gHttp.pid = -1;
        if (pid == gDownload.pid) gDownload.pid = -1;

        fprintf(stderr, "\n[manager] %s (pid %d) exited with %d -- shutting down\n", who, pid,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        cleanup();
        return 1;
    }

    cleanup();
    return 0;
}

// Runs servicemanager in place of this process.
int execServiceManager() {
    const std::string path = findBinary("servicemanager");
    if (path.empty()) {
        fprintf(stderr, "[manager] could not find servicemanager -- has the project been built?\n");
        return 1;
    }
    execl(path.c_str(), path.c_str(), nullptr);
    fprintf(stderr, "[manager] exec %s failed: %s\n", path.c_str(), strerror(errno));
    return 127;
}

} // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";

    if (strcmp(mode, "all") == 0) return runManager();
    if (strcmp(mode, "http") == 0) return httpsvc::runService();
    if (strcmp(mode, "download") == 0) return downloadsvc::runService();
    if (strcmp(mode, "servicemanager") == 0) return execServiceManager();

    fprintf(stderr, "usage: %s [all|http|download|servicemanager]\n", argv[0]);
    return 2;
}
