/*
 * Demo binder service.
 *
 * Registers itself with servicemanager under "demo.service" and serves the
 * IDemoService AIDL interface until killed.
 *
 *   ProcessState::self()          opens /dev/binder and mmaps the buffer
 *   defaultServiceManager()       binder handle 0, i.e. servicemanager
 *   addService(name, this)        publishes us in servicemanager's registry
 *   startThreadPool()/joinThreadPool()  serves incoming transactions
 *
 * If no servicemanager is running this starts one, so the service is usable on
 * its own (from CLion, say) and not only under binder_demo.
 */

#include <com/example/demo/BnDemoService.h>
#include <com/example/demo/IDemoCallback.h>

#include "demo/binary_paths.h"
#include "demo/child_process.h"

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <utils/String16.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

using android::IPCThreadState;
using android::OK;
using android::ProcessState;
using android::sp;
using android::String16;
using android::binder::Status;
using android::os::ParcelFileDescriptor;
using com::example::demo::BnDemoService;
using com::example::demo::IDemoCallback;

namespace {

constexpr char kServiceName[] = "demo.service";

class DemoService : public BnDemoService {
public:
    Status add(int32_t a, int32_t b, int32_t* out) override {
        count();
        logCaller("add(%d, %d)", a, b);
        *out = a + b;
        return Status::ok();
    }

    Status echo(const std::string& message, std::string* out) override {
        count();
        logCaller("echo(\"%s\")", message.c_str());
        *out = "echo: " + message;
        return Status::ok();
    }

    Status getServicePid(int32_t* out) override {
        count();
        *out = static_cast<int32_t>(getpid());
        return Status::ok();
    }

    Status sortDescending(const std::vector<int32_t>& values,
                          std::vector<int32_t>* out) override {
        count();
        logCaller("sortDescending(%zu values)", values.size());
        *out = values;
        std::sort(out->begin(), out->end(), std::greater<int32_t>());
        return Status::ok();
    }

    Status registerCallback(const sp<IDemoCallback>& callback) override {
        count();
        if (callback == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER, "null callback");
        }
        std::lock_guard<std::mutex> lock(mLock);
        mCallbacks.push_back(callback);
        logCaller("registerCallback -> %zu registered", mCallbacks.size());
        return Status::ok();
    }

    Status unregisterCallback(const sp<IDemoCallback>& callback) override {
        count();
        std::lock_guard<std::mutex> lock(mLock);
        // Compare the underlying IBinder: a proxy may be a different C++ object.
        const auto target = IDemoCallback::asBinder(callback);
        mCallbacks.erase(std::remove_if(mCallbacks.begin(), mCallbacks.end(),
                                        [&](const sp<IDemoCallback>& cb) {
                                            return IDemoCallback::asBinder(cb) == target;
                                        }),
                         mCallbacks.end());
        return Status::ok();
    }

    Status broadcast(const std::string& event) override {
        count();
        // oneway: the caller has already been released by the driver.
        std::vector<sp<IDemoCallback>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mLock);
            callbacks = mCallbacks;
        }
        printf("[service] broadcast(\"%s\") to %zu callback(s)\n", event.c_str(),
               callbacks.size());
        fflush(stdout);
        for (const auto& cb : callbacks) {
            cb->onEvent(event); // also oneway
        }
        return Status::ok();
    }

    Status openReport(ParcelFileDescriptor* out) override {
        count();
        // tmpfile() gives us an unlinked fd; binder dup()s it into the caller.
        FILE* f = tmpfile();
        if (f == nullptr) {
            return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "tmpfile failed");
        }
        fprintf(f, "report from service pid=%d\n", getpid());
        fprintf(f, "transactions handled: %d\n", mCount.load());
        fflush(f);
        rewind(f);

        const int dupFd = dup(fileno(f));
        fclose(f);
        if (dupFd < 0) {
            return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "dup failed");
        }
        *out = ParcelFileDescriptor(android::base::unique_fd(dupFd));
        return Status::ok();
    }

    Status getTransactionCount(int32_t* out) override {
        *out = mCount.load();
        return Status::ok();
    }

private:
    void count() { mCount.fetch_add(1); }

    // Caller identity comes from the kernel, not from the payload.
    template <typename... Args>
    void logCaller(const char* fmt, Args... args) {
        IPCThreadState* ipc = IPCThreadState::self();
        printf("[service] pid=%d uid=%d called ", ipc->getCallingPid(), ipc->getCallingUid());
        printf(fmt, args...);
        printf("\n");
        fflush(stdout);
    }

    std::mutex mLock;
    std::vector<sp<IDemoCallback>> mCallbacks;
    std::atomic<int32_t> mCount{0};
};

// -------------------------------------------------------------------------
// Bringing up servicemanager
//
// defaultServiceManager() never fails when servicemanager is missing -- it
// retries forever, logging "Waiting 1s on context object". getContextObject()
// is the same lookup without the loop, so it works as a one-shot probe: it
// pings binder handle 0 and returns null when nobody holds it.
// -------------------------------------------------------------------------

// Only set when *we* started it; a servicemanager that was already running
// belongs to someone else and must outlive us.
pid_t gSpawnedServiceManager = -1;

bool serviceManagerIsUp() {
    return ProcessState::self()->getContextObject(nullptr) != nullptr;
}

void stopSpawnedServiceManager() {
    demo::terminate(gSpawnedServiceManager);
}

void onSignal(int sig) {
    stopSpawnedServiceManager();
    _exit(128 + sig);
}

bool ensureServiceManager() {
    if (serviceManagerIsUp()) return true;

    const std::string path = demo::findBinary("servicemanager");
    if (path.empty()) {
        fprintf(stderr, "[service] no servicemanager running, and none found to start\n");
        return false;
    }

    printf("[service] no servicemanager running -- starting %s\n", path.c_str());
    fflush(stdout);

    gSpawnedServiceManager = demo::spawn(path);
    if (gSpawnedServiceManager < 0) return false;

    // We own it now: don't leave it behind on exit or on Ctrl+C.
    atexit(stopSpawnedServiceManager);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    for (int i = 0; i < 20; ++i) { // 5s, well past the ~100ms it usually takes
        usleep(250 * 1000);
        if (!demo::stillAlive(gSpawnedServiceManager)) {
            fprintf(stderr, "[service] servicemanager exited immediately\n");
            gSpawnedServiceManager = -1; // already reaped
            return false;
        }
        if (serviceManagerIsUp()) {
            printf("[service] servicemanager running (pid %d)\n", gSpawnedServiceManager);
            fflush(stdout);
            return true;
        }
    }

    fprintf(stderr, "[service] servicemanager did not come up within 5s\n");
    stopSpawnedServiceManager();
    return false;
}

} // namespace

int main() {
    // Opens /dev/binder and mmaps the transaction buffer for this process.
    sp<ProcessState> ps = ProcessState::self();
    ps->setThreadPoolMaxThreadCount(4);

    sp<DemoService> service = sp<DemoService>::make();

    if (!ensureServiceManager()) return 1;

    auto sm = android::defaultServiceManager();

    const auto status = sm->addService(String16(kServiceName), service);
    if (status != OK) {
        fprintf(stderr, "[service] addService(%s) failed: %d\n", kServiceName, status);
        return 1;
    }

    printf("[service] pid=%d registered '%s' with servicemanager\n", getpid(), kServiceName);
    printf("[service] waiting for transactions...\n");
    fflush(stdout);

    ps->startThreadPool();
    IPCThreadState::self()->joinThreadPool(); // serves until killed
    return 0;
}
