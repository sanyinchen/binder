/*
 * <stdatomic.h> shim matching libc++/bionic behaviour.
 *
 * AOSP sources such as libbinder's IMemory.cpp include both <atomic> and
 * <stdatomic.h> and then call std::atomic<T>::load(memory_order_relaxed) with
 * the unqualified C11 spelling.  That compiles on Android because libc++'s
 * <stdatomic.h> simply forwards to <atomic> and re-exports the names into the
 * global namespace.
 *
 * Clang ships the C11 header instead, which defines memory_order_* as a
 * distinct C enum and #defines atomic_thread_fence to a builtin -- breaking
 * both the load() calls and libutils' std::atomic_thread_fence.
 *
 * In C++ this header restores the libc++ behaviour; in C it defers to the real
 * one via #include_next.
 */
#ifndef AOSP_COMPAT_STDATOMIC_H
#define AOSP_COMPAT_STDATOMIC_H

#if defined(__cplusplus)

#include <atomic>

using std::atomic;
using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_consume;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

using std::atomic_bool;
using std::atomic_char;
using std::atomic_int;
using std::atomic_llong;
using std::atomic_long;
using std::atomic_schar;
using std::atomic_short;
using std::atomic_uchar;
using std::atomic_uint;
using std::atomic_ullong;
using std::atomic_ulong;
using std::atomic_ushort;

using std::atomic_int_least8_t;
using std::atomic_int_least16_t;
using std::atomic_int_least32_t;
using std::atomic_int_least64_t;
using std::atomic_uint_least8_t;
using std::atomic_uint_least16_t;
using std::atomic_uint_least32_t;
using std::atomic_uint_least64_t;

using std::atomic_intmax_t;
using std::atomic_intptr_t;
using std::atomic_ptrdiff_t;
using std::atomic_size_t;
using std::atomic_uintmax_t;
using std::atomic_uintptr_t;

using std::atomic_flag;

using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_strong_explicit;
using std::atomic_compare_exchange_weak;
using std::atomic_compare_exchange_weak_explicit;
using std::atomic_exchange;
using std::atomic_exchange_explicit;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_and;
using std::atomic_fetch_and_explicit;
using std::atomic_fetch_or;
using std::atomic_fetch_or_explicit;
using std::atomic_fetch_sub;
using std::atomic_fetch_sub_explicit;
using std::atomic_fetch_xor;
using std::atomic_fetch_xor_explicit;
using std::atomic_flag_clear;
using std::atomic_flag_clear_explicit;
using std::atomic_flag_test_and_set;
using std::atomic_flag_test_and_set_explicit;
using std::atomic_init;
using std::atomic_is_lock_free;
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_signal_fence;
using std::atomic_store;
using std::atomic_store_explicit;
using std::atomic_thread_fence;

#ifndef ATOMIC_VAR_INIT
#define ATOMIC_VAR_INIT(value) (value)
#endif
#ifndef ATOMIC_FLAG_INIT
#define ATOMIC_FLAG_INIT \
    {}
#endif

#else /* C: use the real header */

#include_next <stdatomic.h>

#endif /* __cplusplus */

#endif /* AOSP_COMPAT_STDATOMIC_H */
