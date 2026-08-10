#include "DownloadService.h"

#include <com/service/http/BnHttpProgressCallback.h>

#include <android-base/unique_fd.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ParcelFileDescriptor.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using android::IPCThreadState;
using android::OK;
using android::ProcessState;
using android::sp;
using android::String16;
using android::String8;
using android::base::unique_fd;
using android::binder::Status;
using android::os::ParcelFileDescriptor;
using com::service::download::IDownloadCallback;
using com::service::download::IDownloadService;
using com::service::http::IHttpService;

namespace downloadsvc {
namespace {

using Clock = std::chrono::steady_clock;

// Progress forwarded to the client is thinned out further than what the http
// service reports: a progress bar wants a few updates a second, and each one is
// a transaction across a process boundary.
constexpr int64_t kClientIntervalMs = 200;

// The http service is our sibling process and is normally there instantly.
// Waiting this long without seeing it means it never came up, and at that point
// an error is far more useful than hanging forever.
constexpr int kHttpWaitMs = 10 * 1000;

int64_t millisSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

bool isTerminal(int32_t state) {
    return state == IDownloadService::STATE_COMPLETED || state == IDownloadService::STATE_FAILED ||
           state == IDownloadService::STATE_CANCELED;
}

int64_t fileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return st.st_size;
}

// mkdir -p, applied to the parent directory of `path`.
bool ensureParentDir(const std::string& path, std::string* error) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true; // cwd or the root

    const std::string dir = path.substr(0, slash);
    std::string built;
    size_t pos = 0;
    while (pos <= dir.size()) {
        const size_t next = dir.find('/', pos);
        const size_t end = next == std::string::npos ? dir.size() : next;
        built = dir.substr(0, end);
        if (!built.empty() && mkdir(built.c_str(), 0755) != 0 && errno != EEXIST) {
            *error = "cannot create directory " + built + ": " + strerror(errno);
            return false;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

// -------------------------------------------------------------------------
// Progress relay
//
// To the http service this is just an IHttpProgressCallback; to the client it
// is where progress comes from. In between it turns "bytes of this response"
// into "bytes of this download" (the resumed prefix counts), works out a rate,
// and thins the reporting down to what a client actually needs.
// -------------------------------------------------------------------------
class ProgressRelay : public com::service::http::BnHttpProgressCallback {
public:
    explicit ProgressRelay(std::shared_ptr<DownloadService::Task> task)
        : mTask(std::move(task)) {}

    Status onResponseStart(int32_t /*requestId*/, int32_t statusCode, int64_t startOffset,
                           int64_t totalBytes, const std::string& contentType) override {
        mTask->resumedFrom.store(startOffset);
        mTask->total.store(totalBytes);
        mTask->downloaded.store(startOffset);

        {
            std::lock_guard<std::mutex> lock(mLock);
            mLastBytes = startOffset;
            mLastReport = Clock::now();
        }

        printf("[download] #%d HTTP %d, starting at %lld of %s, type %s\n", mTask->id, statusCode,
               static_cast<long long>(startOffset),
               totalBytes < 0 ? "unknown" : std::to_string(totalBytes).c_str(),
               contentType.empty() ? "-" : contentType.c_str());
        fflush(stdout);

        if (mTask->callback != nullptr) {
            mTask->callback->onStarted(mTask->id, mTask->url, totalBytes, startOffset);
        }
        return Status::ok();
    }

    Status onBytesReceived(int32_t /*requestId*/, int64_t received, int64_t total) override {
        const int64_t done = mTask->resumedFrom.load() + received;
        mTask->downloaded.store(done);
        if (total >= 0) mTask->total.store(total);
        if (mTask->callback == nullptr) return Status::ok();

        int64_t bytesPerSecond = 0;
        {
            // oneway transactions to one binder object are delivered serially,
            // but this rate window spans several of them, so take the lock.
            std::lock_guard<std::mutex> lock(mLock);
            const int64_t sinceMs = millisSince(mLastReport);
            if (sinceMs < kClientIntervalMs) return Status::ok();
            bytesPerSecond = (done - mLastBytes) * 1000 / sinceMs;
            mLastBytes = done;
            mLastReport = Clock::now();
        }

        const int64_t knownTotal = mTask->total.load();
        const int32_t percent = knownTotal > 0
                                        ? static_cast<int32_t>(done * 100 / knownTotal)
                                        : -1;
        mTask->callback->onProgress(mTask->id, done, knownTotal, percent, bytesPerSecond);
        return Status::ok();
    }

private:
    std::shared_ptr<DownloadService::Task> mTask;
    std::mutex mLock;
    int64_t mLastBytes = 0;
    Clock::time_point mLastReport = Clock::now();
};

Status failure(int code, const std::string& message) {
    return Status::fromServiceSpecificError(code, String8(message.c_str()));
}

} // namespace

// -------------------------------------------------------------------------
// Task table
// -------------------------------------------------------------------------

std::shared_ptr<DownloadService::Task> DownloadService::findTask(int32_t id) {
    std::lock_guard<std::mutex> lock(mLock);
    const auto it = mTasks.find(id);
    return it == mTasks.end() ? nullptr : it->second;
}

void DownloadService::finish(const std::shared_ptr<Task>& task, int32_t state,
                             const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        task->message = message;
    }
    // State last: anyone who wakes up to a terminal state also sees the message.
    task->state.store(state);
    mFinished.notify_all();
}

// -------------------------------------------------------------------------
// The http service proxy
// -------------------------------------------------------------------------

sp<IHttpService> DownloadService::httpService(std::string* error) {
    std::lock_guard<std::mutex> lock(mHttpLock);
    if (mHttp != nullptr) {
        if (IHttpService::asBinder(mHttp)->isBinderAlive()) return mHttp;
        // The http service restarted: this proxy points at a corpse, look again.
        mHttp = nullptr;
    }

    auto sm = android::defaultServiceManager();
    const String16 name = IHttpService::SERVICE_NAME();
    // Polling checkService() rather than waitForService(): the latter waits
    // forever for a service that may never appear, and a download wants a
    // definite failure instead.
    for (int waited = 0; waited < kHttpWaitMs; waited += 250) {
        sp<android::IBinder> binder = sm->checkService(name);
        if (binder != nullptr) {
            mHttp = android::interface_cast<IHttpService>(binder);
            return mHttp;
        }
        usleep(250 * 1000);
    }

    *error = "'" + std::string(String8(name).c_str()) + "' is not registered -- the http service "
             "is not running";
    return nullptr;
}

// -------------------------------------------------------------------------
// Worker thread
// -------------------------------------------------------------------------

void DownloadService::runTask(std::shared_ptr<Task> task) {
    task->started = Clock::now();
    task->state.store(IDownloadService::STATE_RUNNING);

    const auto fail = [&](int32_t code, const std::string& message) {
        printf("[download] #%d failed: %s\n", task->id, message.c_str());
        fflush(stdout);
        // An empty .part is not a resume point -- it is litter in someone's
        // download directory. Anything with bytes in it is kept.
        if (fileSize(task->partPath) == 0) unlink(task->partPath.c_str());
        finish(task, IDownloadService::STATE_FAILED, message);
        if (task->callback != nullptr) {
            task->callback->onFailed(task->id, code, message);
        }
    };
    const auto cancelDone = [&] {
        const int64_t done = task->downloaded.load();
        printf("[download] #%d canceled, %lld bytes kept on disk\n", task->id,
               static_cast<long long>(done));
        fflush(stdout);
        finish(task, IDownloadService::STATE_CANCELED, "canceled");
        if (task->callback != nullptr) {
            task->callback->onCanceled(task->id, done);
        }
    };

    std::string error;
    if (!ensureParentDir(task->destPath, &error)) {
        fail(IDownloadService::ERROR_BAD_DESTINATION, error);
        return;
    }

    // A .part left behind by an interrupted attempt *is* the resume point: its
    // length is where this run picks up.
    const int64_t resumeFrom = fileSize(task->partPath);
    task->downloaded.store(resumeFrom);
    task->resumedFrom.store(resumeFrom);

    // No O_TRUNC: resuming is exactly about keeping what is already in there.
    unique_fd fd(open(task->partPath.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
    if (!fd.ok()) {
        fail(IDownloadService::ERROR_IO,
             "cannot open " + task->partPath + ": " + strerror(errno));
        return;
    }

    sp<IHttpService> http = httpService(&error);
    if (http == nullptr) {
        fail(IDownloadService::ERROR_HTTP_SERVICE, error);
        return;
    }

    int32_t requestId = 0;
    Status status = http->newRequestId(&requestId);
    if (!status.isOk()) {
        fail(IDownloadService::ERROR_HTTP_SERVICE,
             "newRequestId failed: " + std::string(status.toString8().c_str()));
        return;
    }
    // Publish the id first, then read the cancel flag. cancel() does it the
    // other way round (flag first, then id), so at least one of the two sees
    // the other -- a transfer cannot sneak past a cancel that already happened.
    task->httpRequestId.store(requestId);
    if (task->canceled.load()) {
        cancelDone();
        return;
    }

    printf("[download] #%d handing request #%d to the http service: %s -> %s (resume at %lld)\n",
           task->id, requestId, task->url.c_str(), task->partPath.c_str(),
           static_cast<long long>(resumeFrom));
    fflush(stdout);

    sp<ProgressRelay> relay = sp<ProgressRelay>::make(task);

    // A dup for the Parcel: the kernel dups it once more into the http service,
    // and we still need our own copy for the final fsync and size check.
    ParcelFileDescriptor sink{unique_fd(dup(fd.get()))};
    if (sink.get() < 0) {
        fail(IDownloadService::ERROR_IO, std::string("dup failed: ") + strerror(errno));
        return;
    }

    int64_t written = 0;
    // Blocks for the whole transfer -- which is why every download gets a
    // thread of its own.
    status = http->fetchToFd(requestId, task->url, sink, resumeFrom, relay, &written);

    if (!status.isOk()) {
        if (task->canceled.load() ||
            (status.serviceSpecificErrorCode() == IHttpService::ERROR_CANCELED)) {
            cancelDone();
            return;
        }
        if (status.exceptionCode() == Status::EX_SERVICE_SPECIFIC) {
            // The transport reported this itself; pass its wording through.
            fail(IDownloadService::ERROR_HTTP, status.exceptionMessage().c_str());
        } else {
            // The transaction itself did not land: the http service died, or
            // the interfaces do not match.
            fail(IDownloadService::ERROR_HTTP_SERVICE,
                 "fetchToFd transaction failed: " + std::string(status.toString8().c_str()));
        }
        return;
    }

    const int64_t totalOnDisk = task->resumedFrom.load() + written;
    task->downloaded.store(totalOnDisk);
    fsync(fd.get());
    fd.reset();

    // Rename last: once destPath exists, it is guaranteed to be complete.
    if (rename(task->partPath.c_str(), task->destPath.c_str()) != 0) {
        fail(IDownloadService::ERROR_IO, "rename " + task->partPath + " -> " + task->destPath +
                                                 " failed: " + strerror(errno));
        return;
    }

    const int64_t elapsed = millisSince(task->started);
    const int64_t finalSize = fileSize(task->destPath);
    task->total.store(finalSize);

    printf("[download] #%d complete: %s, %lld bytes in %lld ms\n", task->id,
           task->destPath.c_str(), static_cast<long long>(finalSize),
           static_cast<long long>(elapsed));
    fflush(stdout);

    finish(task, IDownloadService::STATE_COMPLETED, "completed");
    if (task->callback != nullptr) {
        task->callback->onCompleted(task->id, task->destPath, finalSize, elapsed);
    }
}

// -------------------------------------------------------------------------
// IDownloadService
// -------------------------------------------------------------------------

Status DownloadService::enqueue(const std::string& url, const std::string& destPath,
                                const sp<IDownloadCallback>& callback, int32_t* _aidl_return) {
    *_aidl_return = -1;

    if (url.empty()) {
        return failure(IDownloadService::ERROR_HTTP, "empty url");
    }
    if (destPath.empty() || destPath.back() == '/') {
        return failure(IDownloadService::ERROR_BAD_DESTINATION,
                       "destPath must name a file: \"" + destPath + "\"");
    }

    auto task = std::make_shared<Task>();
    task->url = url;
    task->destPath = destPath;
    task->partPath = destPath + ".part";
    task->callback = callback;
    task->state.store(IDownloadService::STATE_PENDING);

    {
        std::lock_guard<std::mutex> lock(mLock);
        task->id = mNextId++;
        mTasks[task->id] = task;
        mOrder.push_back(task->id);
    }

    IPCThreadState* ipc = IPCThreadState::self();
    printf("[download] pid=%d uid=%d enqueue(#%d, \"%s\" -> \"%s\")\n", ipc->getCallingPid(),
           ipc->getCallingUid(), task->id, url.c_str(), destPath.c_str());
    fflush(stdout);

    // Detached: the task is owned by the shared_ptr and released when the
    // thread ends, while the service itself lives until the process is killed.
    std::thread([this, task] { runTask(task); }).detach();

    *_aidl_return = task->id;
    return Status::ok();
}

Status DownloadService::cancel(int32_t downloadId) {
    const std::shared_ptr<Task> task = findTask(downloadId);
    if (task == nullptr || isTerminal(task->state.load())) return Status::ok();

    // Flag first: the worker checks it just before calling fetchToFd().
    task->canceled.store(true);

    const int32_t requestId = task->httpRequestId.load();
    if (requestId >= 0) {
        std::string error;
        sp<IHttpService> http = httpService(&error);
        // oneway, so this does not wait for the http service to come out of its
        // socket read.
        if (http != nullptr) http->cancel(requestId);
    }
    printf("[download] #%d cancel requested\n", downloadId);
    fflush(stdout);
    return Status::ok();
}

Status DownloadService::getState(int32_t downloadId, int32_t* _aidl_return) {
    const std::shared_ptr<Task> task = findTask(downloadId);
    *_aidl_return = task == nullptr ? IDownloadService::STATE_UNKNOWN : task->state.load();
    return Status::ok();
}

Status DownloadService::getDownloadedBytes(int32_t downloadId, int64_t* _aidl_return) {
    const std::shared_ptr<Task> task = findTask(downloadId);
    *_aidl_return = task == nullptr ? -1 : task->downloaded.load();
    return Status::ok();
}

Status DownloadService::awaitCompletion(int32_t downloadId, int64_t timeoutMs,
                                        int32_t* _aidl_return) {
    std::shared_ptr<Task> task = findTask(downloadId);
    if (task == nullptr) {
        *_aidl_return = IDownloadService::STATE_UNKNOWN;
        return Status::ok();
    }

    std::unique_lock<std::mutex> lock(mLock);
    const auto done = [&task] { return isTerminal(task->state.load()); };
    if (timeoutMs <= 0) {
        mFinished.wait(lock, done);
    } else {
        mFinished.wait_for(lock, std::chrono::milliseconds(timeoutMs), done);
    }
    *_aidl_return = task->state.load();
    return Status::ok();
}

Status DownloadService::describe(int32_t downloadId, std::string* _aidl_return) {
    const std::shared_ptr<Task> task = findTask(downloadId);
    if (task == nullptr) {
        *_aidl_return = "#" + std::to_string(downloadId) + " unknown";
        return Status::ok();
    }

    static const char* kStateNames[] = {"pending", "running", "completed", "failed", "canceled"};
    const int32_t state = task->state.load();
    const int64_t done = task->downloaded.load();
    const int64_t total = task->total.load();

    std::string text = "#" + std::to_string(task->id) + " " +
                       (state >= 0 && state <= 4 ? kStateNames[state] : "unknown") + " " +
                       std::to_string(done) + "/" + (total < 0 ? "?" : std::to_string(total)) +
                       " bytes " + task->url + " -> " + task->destPath;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!task->message.empty()) text += " (" + task->message + ")";
    }
    *_aidl_return = text;
    return Status::ok();
}

Status DownloadService::getDownloadIds(std::vector<int32_t>* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    *_aidl_return = mOrder;
    return Status::ok();
}

// -------------------------------------------------------------------------
// Process entry point
// -------------------------------------------------------------------------

int runService() {
    sp<ProcessState> ps = ProcessState::self();
    // The pool serves both client calls and the progress callbacks coming back
    // from the http service. The transfers themselves run on their own worker
    // threads and do not use it.
    ps->setThreadPoolMaxThreadCount(8);
    // Pool first, then register: transactions can arrive the moment the service
    // is in the registry.
    ps->startThreadPool();

    sp<DownloadService> service = sp<DownloadService>::make();
    const String16 name = IDownloadService::SERVICE_NAME();
    const auto status = android::defaultServiceManager()->addService(name, service);
    if (status != OK) {
        fprintf(stderr, "[download] addService(%s) failed: %d\n", String8(name).c_str(), status);
        return 1;
    }

    printf("[download] pid=%d registered '%s'\n", getpid(), String8(name).c_str());
    fflush(stdout);

    IPCThreadState::self()->joinThreadPool();
    return 0;
}

} // namespace downloadsvc
