/* Host compatibility shim for libselinux's Android helpers. */
#pragma once

#include <selinux/label.h>
#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

struct selabel_handle* selinux_android_service_context_handle(void);
struct selabel_handle* selinux_android_vendor_service_context_handle(void);

int selinux_log_callback(int type, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int selinux_vendor_log_callback(int type, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
