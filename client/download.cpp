/*
 * download_client -- downloads a file through download.service.
 *
 * The client does no networking and does not know the http service exists: it
 * looks "download.service" up in servicemanager, hands it a job, and waits for
 * progress to be pushed back.
 *
 *   download_client                          client process
 *        | enqueue(url, path, callback)      binder call
 *   download.service                         download service process
 *        | fetchToFd(...)                    binder call (service to service)
 *   http.service                             HTTP transport process
 *        |                                   progress travels back, oneway
 *
 * Usage:
 *   download_client [url] [destination] [options]
 *
 * Options:
 *   --cancel-after=<ms>   cancel() this long after starting, to show
 *                         cancellation and the resume that follows it
 *   --quiet               no progress bar, only the terminal state
 *
 * With no url at all it downloads kDefaultUrl, so the binary can be run
 * straight from an IDE with no run configuration to fill in.
 *
 * With no destination the file name is taken from the URL and written under
 * ./downloads/. Relative paths are resolved against *this* process's working
 * directory before being sent, since the service runs somewhere else.
 */

#include <com/service/download/BnDownloadCallback.h>
#include <com/service/download/IDownloadService.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using android::IBinder;
using android::IPCThreadState;
using android::OK;
using android::ProcessState;
using android::sp;
using android::String16;
using android::String8;
using android::wp;
using android::binder::Status;
using com::service::download::BnDownloadCallback;
using com::service::download::IDownloadService;

namespace {

// Used when the client is started with no arguments -- pressing Run in an IDE,
// mostly. Picked for what it exercises rather than for the bytes: 10 MB is long
// enough to watch the progress bar move, the server answers Range requests with
// 206 (so --cancel-after followed by a second run demonstrates resuming), and
// https:// puts the TLS path in play.
constexpr char kDefaultUrl[] = "https://proof.ovh.net/files/10Mb.dat";

struct Options {
    std::string url;
    std::string destPath;
    int cancelAfterMs = 0;
    bool quiet = false;
};

std::string humanBytes(int64_t bytes) {
    if (bytes < 0) return "unknown";
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buf;
}

// The destination is resolved here, not in the service. A relative path would
// otherwise be interpreted against the *service's* working directory -- a
// different process, started from a different place -- so "./out.bin" would
// silently land somewhere the user never looked.
std::string absolutePath(const std::string& path) {
    if (!path.empty() && path.front() == '/') return path;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return path;
    return std::string(cwd) + "/" + path;
}

// Best-effort file name from a URL; download.bin when there is nothing to use.
std::string fileNameFromUrl(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    const size_t query = url.find_first_of("?#", start);
    const std::string path = url.substr(start, query == std::string::npos ? std::string::npos
                                                                          : query - start);
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? "" : path.substr(slash + 1);
    return name.empty() ? "download.bin" : name;
}

// -------------------------------------------------------------------------
// The callback
//
// Implemented here and passed to the download service as a binder reference,
// which the service then calls back into this process. The calls land on binder
// pool threads, not on main(); a condition variable wakes main() when a
// terminal state arrives.
// -------------------------------------------------------------------------
class Callback : public BnDownloadCallback {
public:
    explicit Callback(bool quiet) : mQuiet(quiet) {}

    Status onStarted(int32_t downloadId, const std::string& url, int64_t totalBytes,
                     int64_t resumedFrom) override {
        printf("\n[client] #%d started: %s\n", downloadId, url.c_str());
        printf("[client]   total %s", humanBytes(totalBytes).c_str());
        if (resumedFrom > 0) {
            printf(", resuming at %s (that part is not re-fetched)",
                   humanBytes(resumedFrom).c_str());
        }
        printf("\n");
        fflush(stdout);
        return Status::ok();
    }

    Status onProgress(int32_t /*downloadId*/, int64_t downloadedBytes, int64_t totalBytes,
                      int32_t percent, int64_t bytesPerSecond) override {
        if (mQuiet) return Status::ok();

        // Repainted in place: \r goes back to the start of the line.
        char bar[41];
        const int filled = percent < 0 ? 0 : percent * 40 / 100;
        memset(bar, '=', filled);
        memset(bar + filled, ' ', 40 - filled);
        bar[40] = '\0';

        if (percent >= 0) {
            printf("\r[client] [%s] %3d%%  %s / %s  %s/s   ", bar, percent,
                   humanBytes(downloadedBytes).c_str(), humanBytes(totalBytes).c_str(),
                   humanBytes(bytesPerSecond).c_str());
        } else {
            // The server never said how big it is (no Content-Length, or
            // chunked encoding).
            printf("\r[client] %s so far  %s/s   ", humanBytes(downloadedBytes).c_str(),
                   humanBytes(bytesPerSecond).c_str());
        }
        fflush(stdout);
        return Status::ok();
    }

    Status onCompleted(int32_t downloadId, const std::string& path, int64_t totalBytes,
                       int64_t elapsedMs) override {
        const double seconds = elapsedMs / 1000.0;
        printf("\n[client] #%d complete: %s\n", downloadId, path.c_str());
        printf("[client]   %s in %.2fs, %s/s average\n", humanBytes(totalBytes).c_str(), seconds,
               humanBytes(seconds > 0 ? static_cast<int64_t>(totalBytes / seconds) : 0).c_str());
        fflush(stdout);
        settle(IDownloadService::STATE_COMPLETED);
        return Status::ok();
    }

    Status onFailed(int32_t downloadId, int32_t errorCode, const std::string& message) override {
        printf("\n[client] #%d failed (error %d): %s\n", downloadId, errorCode, message.c_str());
        fflush(stdout);
        settle(IDownloadService::STATE_FAILED);
        return Status::ok();
    }

    Status onCanceled(int32_t downloadId, int64_t downloadedBytes) override {
        printf("\n[client] #%d canceled with %s on disk (the .part file is kept, so running the "
               "same command again resumes)\n",
               downloadId, humanBytes(downloadedBytes).c_str());
        fflush(stdout);
        settle(IDownloadService::STATE_CANCELED);
        return Status::ok();
    }

    // Gives up waiting without a terminal callback -- see DeathRecipient.
    void abandon() { settle(IDownloadService::STATE_FAILED); }

    // Blocks until a terminal callback arrives; returns that state.
    int32_t await() {
        std::unique_lock<std::mutex> lock(mLock);
        mDone.wait(lock, [this] { return mState != IDownloadService::STATE_UNKNOWN; });
        return mState;
    }

private:
    void settle(int32_t state) {
        {
            std::lock_guard<std::mutex> lock(mLock);
            mState = state;
        }
        mDone.notify_all();
    }

    const bool mQuiet;
    std::mutex mLock;
    std::condition_variable mDone;
    int32_t mState = IDownloadService::STATE_UNKNOWN;
};

// Notified by the kernel when the download service process dies.
//
// It also has to end the wait: a dead service will never deliver a terminal
// callback, and without this the client would sit in await() forever.
class DeathRecipient : public IBinder::DeathRecipient {
public:
    explicit DeathRecipient(sp<Callback> callback) : mCallback(std::move(callback)) {}

    void binderDied(const wp<IBinder>& /*who*/) override {
        printf("\n[client] *** download.service died (kernel death notification) ***\n");
        fflush(stdout);
        mCallback->abandon();
    }

private:
    sp<Callback> mCallback;
};

void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [url] [destination] [--cancel-after=<ms>] [--quiet]\n"
            "\n"
            "  The services must already be running: scripts/run.sh in another terminal.\n"
            "  Without a url it downloads %s\n"
            "  Without a destination the file name comes from the URL, under ./downloads/\n",
            argv0, kDefaultUrl);
}

bool parseArgs(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quiet") {
            options->quiet = true;
        } else if (arg.rfind("--cancel-after=", 0) == 0) {
            options->cancelAfterMs = atoi(arg.c_str() + strlen("--cancel-after="));
        } else if (arg.rfind("--", 0) == 0) {
            fprintf(stderr, "[client] unknown option: %s\n", arg.c_str());
            return false;
        } else if (options->url.empty()) {
            options->url = arg;
        } else if (options->destPath.empty()) {
            options->destPath = arg;
        } else {
            fprintf(stderr, "[client] unexpected argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (options->url.empty()) {
        options->url = kDefaultUrl;
        printf("[client] no url given, using the default: %s\n", kDefaultUrl);
    }
    if (options->destPath.empty()) {
        options->destPath = "downloads/" + fileNameFromUrl(options->url);
    }
    options->destPath = absolutePath(options->destPath);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }

    sp<ProcessState> ps = ProcessState::self();
    // Needed first: progress is the service calling back into this process, and
    // without a pool there is nobody to handle it.
    ps->startThreadPool();

    // servicemanager *is* binder handle 0, so there is nothing to talk to until
    // it runs. defaultServiceManager() does not fail in that case -- it retries
    // forever, logging "Waiting 1s on context object on /dev/binder" -- so say
    // up front what that loop would mean.
    printf("[client] connecting to servicemanager"
           " (if this waits, start the services first: scripts/run.sh)\n");
    fflush(stdout);
    auto sm = android::defaultServiceManager();

    const String16 serviceName = IDownloadService::SERVICE_NAME();
    sp<IBinder> binder = sm->checkService(serviceName);
    if (binder == nullptr) {
        fprintf(stderr, "[client] '%s' is not registered -- start the services: scripts/run.sh\n",
                String8(serviceName).c_str());
        return 1;
    }
    sp<IDownloadService> service = android::interface_cast<IDownloadService>(binder);

    printf("[client] pid=%d, downloading through '%s'\n", getpid(),
           String8(serviceName).c_str());
    printf("[client]   %s\n[client]   -> %s\n", options.url.c_str(), options.destPath.c_str());
    fflush(stdout);

    sp<Callback> callback = sp<Callback>::make(options.quiet);

    // Registered before enqueue(): from here on, a service that dies takes the
    // wait down with it instead of stranding us.
    sp<DeathRecipient> recipient = sp<DeathRecipient>::make(callback);
    binder->linkToDeath(recipient);

    int32_t downloadId = -1;
    // This binder reference is kept by the service and is how it reaches back
    // into this process.
    Status status = service->enqueue(options.url, options.destPath, callback, &downloadId);
    if (!status.isOk()) {
        fprintf(stderr, "[client] enqueue failed: %s\n", status.toString8().c_str());
        return 1;
    }
    printf("[client] accepted, download id %d\n", downloadId);
    fflush(stdout);

    // Cancellation demo: the service keeps the .part file, so running the same
    // command again resumes from where this left off.
    std::thread canceller;
    if (options.cancelAfterMs > 0) {
        printf("[client] will cancel() in %d ms\n", options.cancelAfterMs);
        fflush(stdout);
        canceller = std::thread([service, downloadId, ms = options.cancelAfterMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            service->cancel(downloadId); // oneway, returns immediately
        });
    }

    const int32_t finalState = callback->await();
    if (canceller.joinable()) canceller.join();

    // What the service recorded should be the same story the callbacks told.
    std::string summary;
    if (service->describe(downloadId, &summary).isOk()) {
        printf("[client] service says: %s\n", summary.c_str());
    }
    fflush(stdout);

    IPCThreadState::self()->stopProcess();
    return finalState == IDownloadService::STATE_COMPLETED ? 0 : 1;
}
