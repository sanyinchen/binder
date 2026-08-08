# 在普通 Linux 上跑 Android 12 Binder（Docker 环境）

把 AOSP `android-12` 的 binder IPC 栈——`libbinder`、`servicemanager` 及其依赖——用 CMake 基于 glibc 构建，在容器里跑通宿主内核真实的 binder 驱动。

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

```bash
scripts/run.sh                  # 需要时构建镜像，然后跑完整 demo
scripts/run.sh shell            # 交互式 shell，binder 已配置好
scripts/run.sh servicemanager   # 只在前台跑 servicemanager
```

前台模式（`servicemanager`、`service`）用 **Ctrl+C** 退出。脚本带了 `--init`，由 tini 担任 PID 1 并转发信号——否则前台进程自己成为 PID 1，内核会丢弃仍是默认处置的信号，Ctrl+C 将毫无反应，只能从另一个终端 `docker kill`。

容器需要 `--privileged` 和 `-v /lib/modules:/lib/modules:ro`，才能 `modprobe binder_linux` 并挂载 binderfs。`scripts/run.sh` 已经带上了这两项。

### demo 入口

`main.cpp` 编译为 `binder_demo`，是驱动整个 demo 的入口。它拉起 servicemanager 和 `demo_service`，再对它们运行 `demo_client`，三个进程共用同一个终端，所以事务发生时输出是交织的。它只做 fork/exec——binder 代码本身在另外几个二进制里。

```
binder_demo                # servicemanager + 服务 + 客户端（默认）
binder_demo service        # 只跑 demo 服务，前台
binder_demo client         # 只跑 demo 客户端
binder_demo servicemanager # 只跑 servicemanager
```

它会按「自身同目录 → 构建树的 `examples/` → `../bin` → `/usr/local/bin`」的顺序查找同伴二进制，所以在 CLion 的构建目录和镜像里的安装布局下都能用。它要求 `/dev/binder` 已经存在，否则会明确报出来。

### 关于内核

binder 驱动是**内核**组件，而容器共享宿主内核，所以这里没有驱动源码要编。宿主内核需要：

```
CONFIG_ANDROID_BINDER_IPC=m      # 或 =y
CONFIG_ANDROID_BINDERFS=m        # 或 =y
```

Ubuntu/Debian 的 generic 和 OEM 内核这两项都是以模块形式提供的；本项目在 `6.14.0-1015-oem` 上开发验证。检查方式：

```bash
grep -E 'ANDROID_BINDER|ANDROID_BINDERFS' /boot/config-$(uname -r)
```

多数发行版内核设了 `CONFIG_ANDROID_BINDER_DEVICES=""`，所以挂载 binderfs 后只会有 `binder-control`，具体设备必须用 `BINDER_CTL_ADD` ioctl 创建。没有现成工具能做这件事，因此有了 `tools/binderfs_ctl.cpp`。entrypoint 会自动调用它创建 `binder`、`hwbinder`、`vndbinder`。

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
main.cpp                  demo 入口 -> binder_demo
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
docker/                   Dockerfile + entrypoint
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

## 直接在宿主上跑（不用 Docker）

如果宿主内核带 binder（见上文「关于内核」），可以直接在宿主上构建运行——想用 CLion 断点调试整条 binder 链路时，这条路更方便。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 一次性配置 binder 驱动（重启后需重跑）
sudo scripts/setup-binder-host.sh

./build/binder_demo        # 注意：不需要 sudo
```

`setup-binder-host.sh` 会加载 `binder_linux`、挂载 binderfs、创建 binder 设备，并把设备权限设为 `0666`——和 Android 上 `/dev/binder` 的权限一致。因此 demo **以普通用户身份**即可运行，CLion 里直接点 Run 就行，不必以 root 启动 IDE。

撤销：

```bash
sudo scripts/setup-binder-host.sh --teardown
```

### 在 CLion 里运行

`Reload CMake Project` 之后，Run 配置选 `binder_demo`。

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
