/*
 * "http.service" -- HttpClient behind the IHttpService binder interface.
 *
 * The service holds no connections between calls: each fetchToFd() is a socket
 * opened, drained into the caller's fd, and closed. All the per-request state
 * there is is a cancel flag, keyed by the id the caller got from
 * newRequestId().
 *
 * fetchToFd() blocks its binder thread for the whole transfer, which is why the
 * process runs a thread pool of several threads -- downloads in parallel each
 * occupy one.
 */
#pragma once

#include <com/service/http/BnHttpService.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>

namespace httpsvc {

class HttpService : public com::service::http::BnHttpService {
public:
    ::android::binder::Status newRequestId(int32_t* _aidl_return) override;

    ::android::binder::Status getString(const ::std::string& url,
                                        ::std::string* _aidl_return) override;

    ::android::binder::Status fetchToFd(
            int32_t requestId, const ::std::string& url,
            const ::android::os::ParcelFileDescriptor& sink, int64_t resumeFrom,
            const ::android::sp<::com::service::http::IHttpProgressCallback>& callback,
            int64_t* _aidl_return) override;

    ::android::binder::Status cancel(int32_t requestId) override;

    ::android::binder::Status getActiveRequestCount(int32_t* _aidl_return) override;

private:
    // One in-flight (or cancelled-before-it-started) request.
    struct Request {
        std::atomic<bool> canceled{false};
        bool running = false;
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    };

    std::shared_ptr<Request> beginRequest(int32_t id);
    void endRequest(int32_t id);

    std::mutex mLock;
    std::map<int32_t, std::shared_ptr<Request>> mRequests; // guarded by mLock
    std::atomic<int32_t> mNextId{1};
    std::atomic<int32_t> mActiveCount{0};
};

// Registers "http.service" with servicemanager and serves until killed.
// Returns a process exit code; it only returns on failure.
int runService();

} // namespace httpsvc
