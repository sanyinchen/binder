/*
 * Permissive libselinux implementation for this host runtime.
 *
 * See compat/include/selinux/selinux.h for the rationale and the security
 * caveat: every access check is answered "allow".
 */

#include <selinux/android.h>
#include <selinux/avc.h>
#include <selinux/label.h>
#include <selinux/selinux.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// A single synthetic context handed out for every process and every service.
constexpr char kPermissiveContext[] = "u:r:permissive:s0";

// Opaque handle; there is only ever one and it carries no state.
struct selabel_handle_impl {
    int unused;
};
selabel_handle_impl gServiceHandle{0};

union selinux_callback gLogCallback {};
union selinux_callback gAuditCallback {};

bool logChecks() {
    static const bool on = [] {
        const char* v = getenv("BINDER_SELINUX_LOG_CHECKS");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

char* dupContext() {
    return strdup(kPermissiveContext);
}

} // namespace

extern "C" {

int getcon(char** con) {
    if (con == nullptr) return -1;
    *con = dupContext();
    return *con != nullptr ? 0 : -1;
}

int getpidcon(pid_t /*pid*/, char** con) {
    if (con == nullptr) return -1;
    *con = dupContext();
    return *con != nullptr ? 0 : -1;
}

void freecon(char* con) {
    free(con);
}

void selinux_set_callback(int type, union selinux_callback cb) {
    switch (type) {
        case SELINUX_CB_LOG:
            gLogCallback = cb;
            break;
        case SELINUX_CB_AUDIT:
            gAuditCallback = cb;
            break;
        default:
            break;
    }
}

int selinux_status_open(int /*fallback*/) {
    return 0;
}

void selinux_status_close(void) {}

// Never reports a policy reload, so cached label handles stay valid.
int selinux_status_updated(void) {
    return 0;
}

int is_selinux_enabled(void) {
    return 0;
}

int security_getenforce(void) {
    return 0; // permissive
}

int selabel_lookup(struct selabel_handle* /*handle*/, char** con, const char* /*key*/,
                   int /*type*/) {
    if (con == nullptr) return -1;
    *con = dupContext();
    return *con != nullptr ? 0 : -1;
}

void selabel_close(struct selabel_handle* /*handle*/) {}

struct selabel_handle* selinux_android_service_context_handle(void) {
    return reinterpret_cast<struct selabel_handle*>(&gServiceHandle);
}

struct selabel_handle* selinux_android_vendor_service_context_handle(void) {
    return reinterpret_cast<struct selabel_handle*>(&gServiceHandle);
}

int selinux_check_access(const char* scon, const char* tcon, const char* tclass, const char* perm,
                         void* /*auditdata*/) {
    if (logChecks()) {
        fprintf(stderr, "[selinux-stub] allow scon=%s tcon=%s class=%s perm=%s\n",
                scon ? scon : "(null)", tcon ? tcon : "(null)", tclass ? tclass : "(null)",
                perm ? perm : "(null)");
    }
    return 0; // allowed
}

int selinux_log_callback(int type, const char* fmt, ...) {
    if (type != SELINUX_ERROR && type != SELINUX_WARNING && !logChecks()) return 0;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[selinux] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    return 0;
}

int selinux_vendor_log_callback(int type, const char* fmt, ...) {
    if (type != SELINUX_ERROR && type != SELINUX_WARNING && !logChecks()) return 0;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[selinux-vendor] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    return 0;
}

} // extern "C"
