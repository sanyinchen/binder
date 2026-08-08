/*
 * Host compatibility shim for libselinux.
 *
 * The desktop kernels this project runs on have no SELinux policy loaded, so
 * there is nothing meaningful to enforce.  This shim provides the exact API
 * surface that servicemanager's Access.cpp uses and answers every access check
 * with "allow".
 *
 * Security note: this makes servicemanager's per-service SELinux gating a no-op.
 * That matches the environment (no policy, no contexts), but it means the
 * add/find/list permissions that AOSP would enforce are NOT enforced here.
 */
#pragma once

#include <stdlib.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short security_class_t;

/* --- context accessors ------------------------------------------------- */
/* Every process reports the same synthetic context. */
int getcon(char** con);
int getpidcon(pid_t pid, char** con);
void freecon(char* con);

/* --- callbacks --------------------------------------------------------- */
#define SELINUX_CB_LOG      0
#define SELINUX_CB_AUDIT    1
#define SELINUX_CB_VALIDATE 2
#define SELINUX_CB_SETENFORCE 3
#define SELINUX_CB_POLICYLOAD 4

/* log callback levels */
#define SELINUX_ERROR   0
#define SELINUX_WARNING 1
#define SELINUX_INFO    2
#define SELINUX_AVC     3

union selinux_callback {
    int (*func_log)(int type, const char* fmt, ...);
    int (*func_audit)(void* auditdata, security_class_t cls, char* msgbuf, size_t msgbufsize);
    int (*func_validate)(char** ctx);
    int (*func_setenforce)(int enforcing);
    int (*func_policyload)(int seqno);
};

void selinux_set_callback(int type, union selinux_callback cb);

/* --- status page ------------------------------------------------------- */
int selinux_status_open(int fallback);
void selinux_status_close(void);
int selinux_status_updated(void);

/* --- enforcement ------------------------------------------------------- */
int is_selinux_enabled(void);
int security_getenforce(void);

#ifdef __cplusplus
}
#endif
