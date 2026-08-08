# 在普通 Linux 上跑 Android 12 Binder

把 AOSP `android-12` 的 binder IPC 栈——`libbinder`、`servicemanager` 及其依赖——用 CMake 基于 glibc 构建，直接在宿主机上跑通内核真实的 binder 驱动。没有容器，没有虚拟机。

不是模拟：事务真正走 `/dev/binder` 和内核的 `binder_linux` 驱动，和 Android 上是同一条路径。

```
$ scripts/run.sh

=== servicemanager: list registered services ===
[client]   demo.service
[client]   manager

=== synchronous call: add ===
[service] pid=19 uid=0 called add(17, 25)
[client] add(17, 25) = 42

=== process identity ===
[client] client pid=19, service pid=17 (separate processes)

=== binder reference: register callback + oneway broadcast ===
[client] broadcast() returned immediately (oneway)
[service] broadcast("system-event-42") to 1 callback(s)
[client] callback fired in pid=19: "system-event-42"

=== fd passing: openReport ===
[client] received fd=5 from service, contents:
[client]   | report from service pid=17

[demo] SUCCESS
```

## 快速开始

前置条件只有两个：内核带 binder（见下文「关于内核」），以及装了 clang。

```bash
sudo apt update && sudo apt install --no-install-recommends clang cmake ninja-build

scripts/run.sh                  # 构建 + 配置 binder + 跑完整 demo
scripts/run.sh servicemanager   # 只在前台跑 servicemanager
scripts/run.sh service          # 只在前台跑 demo 服务
scripts/run.sh client           # 只跑一次 demo 客户端
```

`run.sh` 第一次会在 `build/`（可用 `BUILD_DIR` 覆盖）配置并构建，之后只做增量构建。如果 `/dev/binder` 还不存在，它会用 `sudo` 调 `scripts/setup-binder-host.sh` 把驱动准备好——**只有这一步需要 root**，demo 本身以普通用户身份运行。

前台模式用 **Ctrl+C** 退出。`service` 和 `client` 都假定 servicemanager 已经在跑（先在另一个终端 `scripts/run.sh servicemanager`），否则会一直打印 `Waiting 1s on context object on /dev/binder`。

### 手动构建

不想用脚本包一层的话：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

sudo scripts/setup-binder-host.sh   # 一次性配置 binder 驱动（重启后需重跑）

./build/binder_demo                 # 注意：不需要 sudo
```

`setup-binder-host.sh` 会加载 `binder_linux`、挂载 binderfs、创建 binder 设备，并把设备权限设为 `0666`——和 Android 上 `/dev/binder` 的权限一致。因此 demo 以普通用户身份即可运行，CLion 里直接点 Run 就行，不必以 root 启动 IDE。

撤销：

```bash
sudo scripts/setup-binder-host.sh --teardown
```

### demo 入口

`main.cpp` + `demo/` 编译为 `binder_demo`，是驱动整个 demo 的入口。它拉起 servicemanager 和 `demo_service`，再对它们运行 `demo_client`，三个进程共用同一个终端，所以事务发生时输出是交织的。它只做 fork/exec——binder 代码本身在另外几个二进制里。

`main.cpp` 只负责参数分发，实际逻辑拆在 `demo/` 下：`binary_paths`（找同伴二进制）、`child_process`（fork/exec/wait）、`runner`（demo 编排）。

```
binder_demo                # servicemanager + 服务 + 客户端（默认）
binder_demo service        # 只跑 demo 服务，前台
binder_demo client         # 只跑 demo 客户端
binder_demo servicemanager # 只跑 servicemanager
```

它会按「自身同目录 → 构建树的 `examples/` → `../bin` → `/usr/local/bin`」的顺序查找同伴二进制，所以在 CLion 的构建目录和 `cmake --install` 的安装布局下都能用。它要求 `/dev/binder` 已经存在，否则会明确报出来。

### 关于内核

binder 驱动是**内核**组件，用的就是你正在跑的这个内核，所以这里没有驱动源码要编。内核需要：

```
CONFIG_ANDROID_BINDER_IPC=m      # 或 =y
CONFIG_ANDROID_BINDERFS=m        # 或 =y
```

Ubuntu/Debian 的 generic 和 OEM 内核这两项都是以模块形式提供的；本项目在 `6.14.0-1015-oem` 上开发验证。检查方式：

```bash
grep -E 'ANDROID_BINDER|ANDROID_BINDERFS' /boot/config-$(uname -r)
```

多数发行版内核设了 `CONFIG_ANDROID_BINDER_DEVICES=""`，所以挂载 binderfs 后只会有 `binder-control`，具体设备必须用 `BINDER_CTL_ADD` ioctl 创建。没有现成工具能做这件事，因此有了 `tools/binderfs_ctl.cpp`。`setup-binder-host.sh` 会自动调用它创建 `binder`、`hwbinder`、`vndbinder`。

## demo 覆盖了什么

`examples/aidl/com/example/demo/IDemoService.aidl` 在构建时由项目内置的 AIDL 编译器生成，demo 演示了：

| 能力 | 说明 |
|---|---|
| 服务注册 | `addService()` 注册到 servicemanager |
| 服务发现 | `listServices()`、`waitForService()`、`interface_cast` |
| 同步调用 | 基本类型、UTF-8 字符串、`int[]` 出入参 |
| binder 引用传递 | 客户端把回调 binder 传给服务，服务反向调用回客户端进程 |
| oneway | `broadcast()` 在服务处理完之前就返回 |
| fd 传递 | 服务返回 `ParcelFileDescriptor`，内核把 fd dup 进客户端 |
| 调用方身份 | `getCallingPid()` / `getCallingUid()`，由驱动提供而非报文携带 |
| 死亡通知 | 对服务代理调用 `linkToDeath()` |

## 目录结构

```
main.cpp                  demo 入口（只做参数分发）-> binder_demo
demo/binary_paths.*       定位 servicemanager / demo_service / demo_client
demo/child_process.*      fork / exec / wait 封装
demo/runner.*             demo 本身：拉起服务、跑客户端、收尾
CMakeLists.txt            顶层构建（所有库 + servicemanager）
external/android/         AOSP 源码，逐字节复制（见 PATCHES.md）
  frameworks/native/libs/binder/       libbinder
  frameworks/native/cmds/servicemanager/
  system/core/{libutils,libcutils,libsystem,libprocessgroup}/
  system/logging/liblog/  system/libbase/  external/fmtlib/
  bionic/libc/kernel/uapi/linux/android/   binder.h、binderfs.h
generated/aidl/           libbinder 自身的 AIDL，已预生成
compat/                   libselinux + libvintf 替代实现，以及 host 构建垫片
examples/                 IDemoService.aidl、demo_service.cpp、demo_client.cpp
tools/bin/aidl            AOSP 预编译 AIDL 编译器（含 tools/lib64/libc++.so）
tools/binderfs_ctl.cpp    通过 BINDER_CTL_ADD 创建 binder 设备
patches/                  两个被修改的 AOSP 文件的 diff
scripts/                  run.sh（构建 + 跑 demo）、setup-binder-host.sh（配置驱动）
```

项目是自包含的：构建和运行时都不引用原始 Android 源码树。

## 编译器：必须用 clang

AOSP 只用 clang 构建 libbinder（其 `Android.bp` 里写了 `clang: true`），代码里依赖了若干 GCC 会直接报错的 clang 宽容写法：

* `utils/Vector.h`、`utils/SortedVector.h` 里声明了 `const operator=` 重载，函数体却调用非 const 的 `VectorImpl::operator=`。clang 把错误推迟到实例化时（而实际上从没有人实例化过它们），GCC 在解析阶段就报错。
* `binder/Parcel.h` 里约 9 个**函数定义**把 `__attribute__((deprecated(...)))` 放在参数列表和函数体之间。GCC 只接受把属性放在声明上。

所以顶层 `CMakeLists.txt` 会强制检查编译器：**新建**构建目录时会自动探测并选用 clang；如果被指定成了 GCC，配置阶段就会失败并给出修复指引，而不是让你面对几十屏模板报错。

在 CLion 里：`Settings > Build, Execution, Deployment > Toolchains`，把 C Compiler 设为 `clang`、C++ Compiler 设为 `clang++`，然后 `File > Reload CMake Project`。如果之前用 GCC 配置过，先删掉 `cmake-build-debug/`——CMake 不允许在已有缓存里换编译器。

```bash
sudo apt update      # 索引过期会导致 404，这一步不能省
sudo apt install --no-install-recommends clang
```

`--no-install-recommends` 可以跳过 `libclang-rt-18-dev` 拖进来的 32 位运行时（约 500MB），本项目用不到。`lld` 也不需要。

## 在 CLion 里运行

因为整条 binder 链路就跑在宿主机上，断点调试不需要任何远程配置：`Reload CMake Project` 之后，Run 配置选 `binder_demo` 即可。前提是先 `sudo scripts/setup-binder-host.sh` 配好驱动（重启后需重跑），之后不必以 root 启动 IDE。

`demo_service` 可以单独 Run：它发现没有 servicemanager 时会自己拉起一个（并在自己退出时把它一起收掉；如果 servicemanager 本来就在跑，则不动它）。

**`demo_client` 不能单独 Run**——它还需要 `demo.service` 已注册，所以要么先 Run `demo_service`，要么直接 Run `binder_demo`。否则会一直卡在：

```
ProcessState W  Not able to get context object on /dev/binder.
ServiceManager E  Waiting 1s on context object on /dev/binder.
```

这个循环**不会**超时退出：`defaultServiceManager()` 在 android-12 里是无限重试，不返回 null。`demo_service` 用的是同一次查找的非阻塞版本 `ProcessState::getContextObject()`——它 ping 一下 handle 0，没人占着就返回 null，所以才能拿它当探测手段。

注意 CLion 默认只构建被选中的那个 target，而 `binder_demo` 在运行期需要 `servicemanager`、`demo_service`、`demo_client` 三个同伴二进制（CMake 里已用 `add_dependencies` 声明，会一并构建）。`binderfs_ctl` 只被配置脚本用到，需要时单独构建：

```bash
cmake --build cmake-build-debug --target binderfs_ctl
```

## 重新生成 libbinder 的 AIDL

`generated/aidl/` 已经入库，构建时无需额外步骤，但可以重新生成：

```bash
B=external/android/frameworks/native/libs/binder
./tools/bin/aidl --lang=cpp -I $B/aidl \
  --header_out=generated/aidl --out=generated/aidl \
  $B/aidl/android/os/*.aidl $B/aidl/android/content/pm/*.aidl
```

## 已知限制

* **SELinux 强制被桩掉了。** servicemanager 的按服务访问控制一律放行，详见 PATCHES.md。
* **VINTF 声明**来自文本文件，而非真正的 VINTF XML。
* **没有 logd。** liblog 以 host 模式构建，`ALOG*` 输出到 stderr。
* **Ashmem** 用的是 libcutils 的 host 实现（已 unlink 的临时文件），不是 `/dev/ashmem` 或 memfd。
* `libbinder` 的 RPC-over-sockets 代码编进去了，但 demo 没有演示。
