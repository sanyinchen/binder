/*
 * "download.service" -- a download manager built on top of "http.service".
 *
 * This is the service-calls-service layer of the project: one enqueue() makes
 * this service look IHttpService up in servicemanager and issue a binder call
 * to it, and the http service then calls progress back over binder. The client
 * never sees that the http service exists at all.
 *
 * Each download runs on its own worker thread: fetchToFd() blocks, and
 * enqueue() has to return right away. Everything after that reaches the client
 * process through IDownloadCallback (oneway).
 */
#pragma once

#include <com/service/download/BnDownloadService.h>
#include <com/service/http/IHttpService.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace downloadsvc {

class DownloadService : public com::service::download::BnDownloadService {
public:
    // Everything one download knows about itself. Held by shared_ptr: the
    // worker thread, the progress relay and the service all reference it, and
    // which of them lets go last is not fixed.
    struct Task {
        int32_t id = 0;
        std::string url;
        std::string destPath;
        std::string partPath; // destPath + ".part", what the transfer writes to

        ::android::sp<::com::service::download::IDownloadCallback> callback;

        std::atomic<int32_t> state{0};       // STATE_*
        std::atomic<int64_t> downloaded{0};  // bytes on disk, resumed prefix included
        std::atomic<int64_t> total{-1};      // resource size, -1 when unknown
        std::atomic<int64_t> resumedFrom{0}; // how much was picked up, not re-fetched
        std::atomic<int32_t> httpRequestId{-1};
        std::atomic<bool> canceled{false};

        std::chrono::steady_clock::time_point started;
        std::string message; // why it ended; guarded by the service's mLock
    };

    ::android::binder::Status enqueue(
            const ::std::string& url, const ::std::string& destPath,
            const ::android::sp<::com::service::download::IDownloadCallback>& callback,
            int32_t* _aidl_return) override;

    ::android::binder::Status cancel(int32_t downloadId) override;

    ::android::binder::Status getState(int32_t downloadId, int32_t* _aidl_return) override;

    ::android::binder::Status getDownloadedBytes(int32_t downloadId,
                                                 int64_t* _aidl_return) override;

    ::android::binder::Status awaitCompletion(int32_t downloadId, int64_t timeoutMs,
                                              int32_t* _aidl_return) override;

    ::android::binder::Status describe(int32_t downloadId, ::std::string* _aidl_return) override;

    ::android::binder::Status getDownloadIds(::std::vector<int32_t>* _aidl_return) override;

private:
    // Worker thread body: prepare the .part file, drive the http service, and
    // finish up.
    void runTask(std::shared_ptr<Task> task);

    // The http service proxy, waiting for it to register if need be. Returns
    // null and fills `error` when it never shows up.
    ::android::sp<::com::service::http::IHttpService> httpService(std::string* error);

    std::shared_ptr<Task> findTask(int32_t id);
    // Records the terminal state and reason, and wakes anyone in
    // awaitCompletion().
    void finish(const std::shared_ptr<Task>& task, int32_t state, const std::string& message);

    std::mutex mLock;
    std::condition_variable mFinished; // signalled when a task reaches a terminal state
    std::map<int32_t, std::shared_ptr<Task>> mTasks;
    std::vector<int32_t> mOrder; // ids in enqueue order
    int32_t mNextId = 1;

    std::mutex mHttpLock;
    ::android::sp<::com::service::http::IHttpService> mHttp; // cache, guarded by mHttpLock
};

// Registers "download.service" with servicemanager and serves until killed.
int runService();

} // namespace downloadsvc
