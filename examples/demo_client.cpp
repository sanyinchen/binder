/*
 * Demo binder client.
 *
 * Looks "demo.service" up through servicemanager and exercises every
 * transaction shape the service offers, then shows servicemanager's own
 * registry APIs and death notification.
 */

#include <com/example/demo/BnDemoCallback.h>
#include <com/example/demo/IDemoService.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include <chrono>
#include <cstdio>
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
using android::os::ParcelFileDescriptor;
using com::example::demo::BnDemoCallback;
using com::example::demo::IDemoService;

namespace {

constexpr char kServiceName[] = "demo.service";

void section(const char* title) {
    printf("\n=== %s ===\n", title);
    fflush(stdout);
}

// Implemented by the client; the service holds a proxy to it.
class Callback : public BnDemoCallback {
public:
    Status onEvent(const std::string& event) override {
        printf("[client] callback fired in pid=%d: \"%s\"\n", getpid(), event.c_str());
        fflush(stdout);
        return Status::ok();
    }
};

// Notified by the kernel when the service process dies.
class DeathRecipient : public IBinder::DeathRecipient {
public:
    void binderDied(const wp<IBinder>& /*who*/) override {
        printf("[client] *** service died (kernel death notification) ***\n");
        fflush(stdout);
    }
};

} // namespace

int main() {
    sp<ProcessState> ps = ProcessState::self();
    // Needed so the service can call back into this process.
    ps->startThreadPool();

    // servicemanager *is* binder handle 0, so there is nothing to talk to until
    // it runs. defaultServiceManager() does not fail in that case -- it retries
    // forever, logging "Waiting 1s on context object on /dev/binder" -- so say
    // up front what that loop would mean. Unlike demo_service the client does
    // not start anything itself: it needs demo.service too, and demo_service
    // brings up servicemanager on its own.
    printf("[client] connecting to servicemanager"
           " (if this waits, start the service first: scripts/run.sh service)\n");
    fflush(stdout);
    auto sm = android::defaultServiceManager();

    section("servicemanager: list registered services");
    for (const String16& name : sm->listServices()) {
        printf("[client]   %s\n", String8(name).c_str());
    }

    section("servicemanager: look up demo.service");
    // waitForService blocks until the service registers (or times out).
    sp<IBinder> binder = sm->waitForService(String16(kServiceName));
    if (binder == nullptr) {
        fprintf(stderr, "[client] '%s' not found\n", kServiceName);
        return 1;
    }
    sp<IDemoService> service = android::interface_cast<IDemoService>(binder);
    printf("[client] got proxy for '%s'\n", kServiceName);

    section("death notification: register recipient");
    sp<DeathRecipient> recipient = sp<DeathRecipient>::make();
    if (binder->linkToDeath(recipient) != OK) {
        fprintf(stderr, "[client] linkToDeath failed\n");
    } else {
        printf("[client] linkToDeath registered\n");
    }

    section("synchronous call: add");
    int32_t sum = 0;
    if (service->add(17, 25, &sum).isOk()) {
        printf("[client] add(17, 25) = %d\n", sum);
    }

    section("strings: echo");
    std::string echoed;
    if (service->echo("hello from client", &echoed).isOk()) {
        printf("[client] echo -> \"%s\"\n", echoed.c_str());
    }

    section("process identity");
    int32_t servicePid = 0;
    if (service->getServicePid(&servicePid).isOk()) {
        printf("[client] client pid=%d, service pid=%d (separate processes)\n", getpid(),
               servicePid);
    }

    section("arrays: sortDescending");
    std::vector<int32_t> input{5, 3, 9, 1, 7};
    std::vector<int32_t> sorted;
    if (service->sortDescending(input, &sorted).isOk()) {
        printf("[client] sorted:");
        for (int32_t v : sorted) printf(" %d", v);
        printf("\n");
    }

    section("binder reference: register callback + oneway broadcast");
    sp<Callback> callback = sp<Callback>::make();
    if (service->registerCallback(callback).isOk()) {
        printf("[client] callback registered\n");
    }
    // oneway -- returns before the service has handled it.
    service->broadcast("system-event-42");
    printf("[client] broadcast() returned immediately (oneway)\n");
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the callback land

    section("fd passing: openReport");
    ParcelFileDescriptor pfd;
    if (service->openReport(&pfd).isOk() && pfd.get() >= 0) {
        printf("[client] received fd=%d from service, contents:\n", pfd.get());
        char buf[512];
        ssize_t n;
        lseek(pfd.get(), 0, SEEK_SET);
        while ((n = read(pfd.get(), buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("[client]   | %s", buf);
        }
        fflush(stdout);
    }

    section("transaction count");
    int32_t total = 0;
    if (service->getTransactionCount(&total).isOk()) {
        printf("[client] service handled %d transactions\n", total);
    }

    service->unregisterCallback(callback);

    printf("\n[client] all checks passed\n");
    fflush(stdout);

    IPCThreadState::self()->stopProcess();
    return 0;
}
