#include "HttpService.h"

#include "HttpClient.h"

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using android::IPCThreadState;
using android::OK;
using android::ProcessState;
using android::sp;
using android::String16;
using android::String8;
using android::binder::Status;
using android::os::ParcelFileDescriptor;
using com::service::http::IHttpProgressCallback;
using com::service::http::IHttpService;

namespace httpsvc {
namespace {

using Clock = std::chrono::steady_clock;

// getString() is for small bodies; anything bigger belongs in a file.
constexpr size_t kMaxStringResponse = 1024 * 1024;

// Progress is reported when either threshold is crossed, so a fast transfer
// does not flood the caller with binder traffic and a slow one still ticks.
constexpr int64_t kProgressIntervalMs = 150;
constexpr int64_t kProgressBytes = 256 * 1024;

// A pre-cancelled id nobody ever uses would sit in the map forever; drop the
// ones that were never claimed.
constexpr int64_t kStaleRequestMs = 60 * 1000;

int64_t millisSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void logCaller(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logCaller(const char* fmt, ...) {
    // Caller identity comes from the kernel, not from the payload.
    IPCThreadState* ipc = IPCThreadState::self();
    printf("[http] pid=%d uid=%d ", ipc->getCallingPid(), ipc->getCallingUid());
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

// write(2) is allowed to write less than asked for.
bool writeFully(int fd, const char* data, size_t len) {
    while (len > 0) {
        const ssize_t n = write(fd, data, len);
        if (n > 0) {
            data += n;
            len -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

Status failure(int code, const std::string& message) {
    return Status::fromServiceSpecificError(code, String8(message.c_str()));
}

} // namespace

// -------------------------------------------------------------------------
// Request bookkeeping
// -------------------------------------------------------------------------

std::shared_ptr<HttpService::Request> HttpService::beginRequest(int32_t id) {
    std::lock_guard<std::mutex> lock(mLock);
    auto it = mRequests.find(id);
    if (it == mRequests.end()) {
        it = mRequests.emplace(id, std::make_shared<Request>()).first;
    }
    // A cancel() that arrived before this call keeps its flag: the transfer is
    // supposed to be dead on arrival, not to start anyway.
    it->second->running = true;
    return it->second;
}

void HttpService::endRequest(int32_t id) {
    std::lock_guard<std::mutex> lock(mLock);
    mRequests.erase(id);
}

// -------------------------------------------------------------------------
// IHttpService
// -------------------------------------------------------------------------

Status HttpService::newRequestId(int32_t* _aidl_return) {
    *_aidl_return = mNextId.fetch_add(1);
    return Status::ok();
}

Status HttpService::getString(const std::string& url, std::string* _aidl_return) {
    logCaller("getString(\"%s\")", url.c_str());

    mActiveCount.fetch_add(1);
    const httpsvc::Result result = httpsvc::getString(url, kMaxStringResponse, _aidl_return);
    mActiveCount.fetch_sub(1);

    if (result.error != kOk) {
        printf("[http]   -> failed: %s\n", result.message.c_str());
        fflush(stdout);
        return failure(result.error, result.message);
    }
    printf("[http]   -> %zu bytes, HTTP %d\n", _aidl_return->size(), result.response.statusCode);
    fflush(stdout);
    return Status::ok();
}

Status HttpService::fetchToFd(int32_t requestId, const std::string& url,
                              const ParcelFileDescriptor& sink, int64_t resumeFrom,
                              const sp<IHttpProgressCallback>& callback, int64_t* _aidl_return) {
    *_aidl_return = 0;

    // The fd was dup'd into this process by the kernel when the Parcel was
    // read; it stays valid for exactly as long as this call.
    const int fd = sink.get();
    if (fd < 0) {
        return failure(IHttpService::ERROR_WRITE, "no file descriptor to write to");
    }
    if (resumeFrom < 0) resumeFrom = 0;

    logCaller("fetchToFd(#%d, \"%s\", resumeFrom=%lld)", requestId, url.c_str(),
              static_cast<long long>(resumeFrom));

    const std::shared_ptr<Request> request = beginRequest(requestId);
    mActiveCount.fetch_add(1);

    httpsvc::Request options;
    options.url = url;
    options.resumeFrom = resumeFrom;

    int64_t total = -1;
    int64_t written = 0;
    int64_t reportedAt = 0;
    auto lastReport = Clock::now();
    std::string writeError;

    const auto result = httpsvc::get(
            options,
            [&](const httpsvc::ResponseInfo& info) {
                total = info.totalBytes;
                // The body belongs at info.startOffset, which is where the
                // server decided to start -- not necessarily where we asked.
                if (lseek(fd, info.startOffset, SEEK_SET) < 0) {
                    writeError = std::string("lseek: ") + strerror(errno);
                    return false;
                }
                if (info.startOffset == 0 && resumeFrom > 0) {
                    // Resume refused: whatever is already in the file is stale.
                    if (ftruncate(fd, 0) < 0) {
                        writeError = std::string("ftruncate: ") + strerror(errno);
                        return false;
                    }
                }
                printf("[http]   #%d HTTP %d, body [%lld..%s) type=%s\n", requestId,
                       info.statusCode, static_cast<long long>(info.startOffset),
                       total < 0 ? "?" : std::to_string(total).c_str(),
                       info.contentType.empty() ? "-" : info.contentType.c_str());
                fflush(stdout);
                if (callback != nullptr) {
                    callback->onResponseStart(requestId, info.statusCode, info.startOffset, total,
                                              info.contentType);
                }
                return true;
            },
            [&](const char* data, size_t len) {
                if (!writeFully(fd, data, len)) {
                    writeError = std::string("write: ") + strerror(errno);
                    return false;
                }
                written += static_cast<int64_t>(len);
                if (callback != nullptr &&
                    (written - reportedAt >= kProgressBytes ||
                     millisSince(lastReport) >= kProgressIntervalMs)) {
                    // oneway: costs the transfer a syscall, not a round trip.
                    callback->onBytesReceived(requestId, written, total);
                    reportedAt = written;
                    lastReport = Clock::now();
                }
                return true;
            },
            [&] { return request->canceled.load(); });

    mActiveCount.fetch_sub(1);
    endRequest(requestId);

    if (result.error != kOk) {
        // A sink failure surfaces as kErrorWrite with a generic message; the
        // specific errno is the one worth reporting.
        const std::string message = writeError.empty() ? result.message : writeError;
        const int code = writeError.empty() ? result.error : IHttpService::ERROR_WRITE;
        printf("[http]   #%d failed after %lld bytes: %s\n", requestId,
               static_cast<long long>(written), message.c_str());
        fflush(stdout);
        return failure(code, message);
    }

    if (callback != nullptr) {
        callback->onBytesReceived(requestId, written, total);
    }
    printf("[http]   #%d done, %lld bytes\n", requestId, static_cast<long long>(written));
    fflush(stdout);

    *_aidl_return = written;
    return Status::ok();
}

Status HttpService::cancel(int32_t requestId) {
    logCaller("cancel(#%d)", requestId);

    std::lock_guard<std::mutex> lock(mLock);
    auto it = mRequests.find(requestId);
    if (it == mRequests.end()) {
        // Cancelling a request that has not called fetchToFd() yet is legal --
        // the id exists as soon as newRequestId() returned it. Remember the
        // cancellation so the transfer never gets off the ground.
        for (auto stale = mRequests.begin(); stale != mRequests.end();) {
            const bool expired = !stale->second->running &&
                                 millisSince(stale->second->created) > kStaleRequestMs;
            stale = expired ? mRequests.erase(stale) : std::next(stale);
        }
        it = mRequests.emplace(requestId, std::make_shared<Request>()).first;
    }
    it->second->canceled.store(true);
    return Status::ok();
}

Status HttpService::getActiveRequestCount(int32_t* _aidl_return) {
    *_aidl_return = mActiveCount.load();
    return Status::ok();
}

// -------------------------------------------------------------------------
// Process entry point
// -------------------------------------------------------------------------

int runService() {
    sp<ProcessState> ps = ProcessState::self();
    // Every fetchToFd() parks a binder thread for the length of the transfer,
    // so the pool size is the number of downloads that can run at once.
    ps->setThreadPoolMaxThreadCount(8);

    sp<HttpService> service = sp<HttpService>::make();
    // The name comes from the AIDL constant; callers look up the same symbol.
    const String16 name = IHttpService::SERVICE_NAME();
    const auto status = android::defaultServiceManager()->addService(name, service);
    if (status != OK) {
        fprintf(stderr, "[http] addService(%s) failed: %d\n", String8(name).c_str(), status);
        return 1;
    }

    printf("[http] pid=%d registered '%s' (https: %s)\n", getpid(), String8(name).c_str(),
           httpsvc::tlsSupported() ? "yes" : "no -- built without OpenSSL");
    fflush(stdout);

    ps->startThreadPool();
    IPCThreadState::self()->joinThreadPool();
    return 0;
}

} // namespace httpsvc
