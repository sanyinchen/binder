# 对 AOSP 源码的本地改动

`external/android/` 下的所有内容都是从 AOSP `android-12` 树逐字节复制过来的。其中只有两个文件带本地改动，两者的 unified diff 都放在 `patches/` 里，且改动都局限在上游仅为 Android 目标编译的代码上。

原始源码树 `/mnt/extra2/AndroidSource/android_12` **未被修改**。

## 1. `frameworks/native/libs/binder/IPCThreadState.cpp`

四处 `#if defined(__ANDROID__)` 守卫改为
`#if defined(__ANDROID__) || defined(BINDER_ENABLE_KERNEL_IPC)`。

上游为 host 编译 libbinder 时是把内核驱动**关掉**的——host 构建的目的是服务基于 socket 的 RPC binder，所以 `talkWithDriver()` 直接返回 `INVALID_OPERATION`，根本不会发 `BINDER_WRITE_READ`。我们要的是真实的 `/dev/binder` 驱动，因此把这些 ioctl 路径重新打开：

| 行号 | 守卫的内容 |
|------|----------------|
| 1027 | `BINDER_WRITE_READ` —— 真正的事务 ioctl |
| 1376 | 线程退出时的 `BINDER_THREAD_EXIT` |
| 1391 | `BINDER_GET_FROZEN_INFO` |
| 1410 | `BINDER_FREEZE` |

## 2. `frameworks/native/libs/binder/ProcessState.cpp`

两处改动。

**a. 同样的守卫翻转**（第 438 行），让打开驱动失败时直接 fatal，而不是静默产出一个无法做事务的进程。

**b. `becomeContextManager()` 在使用扩展 ioctl 前先探测 SELinux。**

这一处是真正的行为修复，不只是构建开关。`BINDER_SET_CONTEXT_MGR_EXT` 会设置 `FLAT_BINDER_FLAG_TXN_SECURITY_CTX`，要求驱动给每一次事务附加调用方的 SELinux 上下文。在没有 SELinux 的内核上——普通桌面发行版、跑 AppArmor 的 Ubuntu 之类，这是常态——这个 ioctl **本身是成功的**，然后之后每一次事务都会在驱动内部失败：

```
binder_linux: 31365:31365 failed to get security context
binder_linux: 31365:31365 transaction call to 31363:0 failed 14/29201/-22, code 1599098439
```

上游已有的「退回 `BINDER_SET_CONTEXT_MGR`」的 fallback 只在 **ioctl 失败**时触发，所以这里根本不会走到。补丁改为先检查 `/sys/fs/selinux`：有 SELinux 时上游行为不变；没有时改用传统 ioctl，不索取安全上下文。

不打这个补丁的症状：`addService()` 返回 `-129`（`Status::EX_TRANSACTION_FAILED`）。

# 被替换而非打补丁的依赖

## `libselinux` → `compat/selinux_stub.cpp`

servicemanager 的每一次 add/find/list 都要过 SELinux。普通桌面内核上没有加载任何策略，所以这个替代实现对所有检查一律回答**放行**，并发放同一个合成上下文。

> **安全提示：** AOSP 原本强制的按服务访问控制在这里是空操作。对于一个没有加载策略的内核来说这是正确的，但**这不是真机 Android 的安全姿态**。设置 `BINDER_SELINUX_LOG_CHECKS=1` 可以追踪本应被评估的每一次检查。

## `libvintf` → `compat/vintf_stub.cpp`

真正的 libvintf 要解析 VINTF XML，并拖进 tinyxml2、libhidlmetadata 和 libfs_mgr。而 servicemanager 只需要知道某个 AIDL 实例是否已声明，所以这个替代实现改从一个按行组织的文本文件来回答（`/etc/binder/vintf_manifest.txt`，可用 `$BINDER_VINTF_MANIFEST` 覆盖）：

```
some.package.IFoo/default
some.package.IBar/instance@com.android.some.apex
```

文件不存在时就是「什么都没声明」，对于一个不提供 VINTF HAL 的系统来说这正是正确答案。只有标记为 vendor stability 的 binder 才会去查它。

# 构建垫片（未触碰任何 AOSP 源码）

* `compat/include/aosp_host_compat.h` —— 强制包含进每个编译单元。AOSP 是针对 bionic 构建的，bionic 的头文件传递包含的东西比 glibc 多得多；没有这个垫片，复制过来的源码会因为 `strerror` 未声明、`std::unique_ptr` 找不到之类的问题编不过。
* `compat/include/stdatomic.h` —— libbinder 的 `IMemory.cpp` 同时包含了 `<atomic>` 和 `<stdatomic.h>`，然后用无限定的 C11 写法调用 `std::atomic<T>::load(memory_order_relaxed)`。libc++ 的 `<stdatomic.h>` 会转发到 `<atomic>`；而 clang 自带的是 C11 版本，它定义了一个冲突的枚举，还把 `atomic_thread_fence` `#define` 掉了。这个垫片恢复 libc++ 的行为。
* fmtlib 以 header-only 方式构建 —— 这个版本的 `src/format.cc` 显式实例化了 `detail::basic_data<void>`，当前 clang 会判定为重复实例化而报错。
* `libcutils/multiuser.cpp` 上游标记为 android-only，但里面全是纯算术，所以为 host 编译了进来。

# 为什么必须用 clang（未改动这些代码）

AOSP 从不用 GCC 构建 libbinder，代码里有若干只在 clang 下成立的写法。这些**没有**被修补，而是由 `CMakeLists.txt` 强制要求 clang：

* `utils/Vector.h:258/270`、`utils/SortedVector.h:185` —— `const operator=` 重载的函数体调用非 const 的 `VectorImpl::operator=`。clang 把错误推迟到实例化时，而这些重载从未被实例化过（真要实例化，clang 同样会报错），所以是死代码；GCC 在解析模板定义时就报
  `cannot convert 'const android::Vector<TYPE>*' to 'android::VectorImpl*'`。
* `binder/Parcel.h` —— 约 9 个**函数定义**把 `__attribute__((deprecated(...)))` 放在参数列表和函数体之间。GCC 只接受属性出现在声明上，报
  `attributes are not allowed on a function-definition`。

修补这些要动十几处上游代码，而且只是在 61 个编译单元里刚跑到第 13 个时暴露出来的问题——后面大概率还有。强制使用 clang 与上游保持一致，也让 `external/android/` 的改动面维持在最小。
