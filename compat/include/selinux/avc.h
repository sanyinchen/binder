/* Host compatibility shim for libselinux's access vector cache. */
#pragma once

#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns 0 (allowed) unconditionally.  See selinux.h for why.
 * Set BINDER_SELINUX_LOG_CHECKS=1 in the environment to trace each check.
 */
int selinux_check_access(const char* scon, const char* tcon, const char* tclass,
                         const char* perm, void* auditdata);

#ifdef __cplusplus
}
#endif
