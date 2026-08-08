/* Host compatibility shim for libselinux's label backend. */
#pragma once

#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SELABEL_CTX_FILE            0
#define SELABEL_CTX_MEDIA           1
#define SELABEL_CTX_X               2
#define SELABEL_CTX_DB              3
#define SELABEL_CTX_ANDROID_PROP    4
#define SELABEL_CTX_ANDROID_SERVICE 5

struct selabel_handle;

/* Always resolves to the permissive default context. */
int selabel_lookup(struct selabel_handle* handle, char** con, const char* key, int type);
void selabel_close(struct selabel_handle* handle);

#ifdef __cplusplus
}
#endif
