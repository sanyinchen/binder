# 在普通 Linux 上跑 Android 12 Binder

把 AOSP `android-12` 的 binder IPC 栈——`libbinder`、`servicemanager` 及其依赖——用 CMake 基于 glibc 构建，直接在宿主机上跑通内核真实的 binder 驱动。没有容器，没有虚拟机。

不是模拟：事务真正走 `/dev/binder` 和内核的 `binder_linux` 驱动，和 Android 上是同一条路径。

在这套 binder 之上跑的是两个真实服务：

```
download_client                      客户端进程
     │ enqueue(url, path, callback)  binder 调用
download.service                     下载服务进程（.part 文件、断点续传、原子改名）
     │ fetchToFd(...)                binder 调用 —— 服务调服务
http.service                         HTTP 传输进程（socket、HTTP/1.1、TLS）
     │                               进度沿原路 oneway 回调回来
```

三个进程之间的每一次调用都是真正的 binder 事务，包括中间那一跳：download service 自己不碰网络，它是 http service 的**客户端**。响应体不走 Parcel——download service 把文件描述符通过 binder 传给 http service，由内核 dup 到对方进程，几百 MB 的下载和几百字节的下载占用的 binder 流量是一样的。

服务端（终端 A）：

```
$ scripts/run.sh

[manager] servicemanager running (pid 52321)
[http] pid=52322 registered 'http.service' (https: yes)
[manager] http service ready (pid 52322)
[download] pid=52324 registered 'download.service'
[manager] download service ready (pid 52324)

[manager] all up. Registered services:
[manager]   download.service
[manager]   http.service
[manager]   manager
[manager] the client can go now: download_client <url> <destination>
[manager] Ctrl+C to stop.
```

客户端（终端 B），中途取消再重跑一次续传：

```
$ scripts/run.sh client http://127.0.0.1:8733/slow.bin ./slow.bin --cancel-after=2000
[client] pid=52892, downloading through 'download.service'
[client] accepted, download id 3

[client] #3 started: http://127.0.0.1:8733/slow.bin
[client]   total 2.9 MB
[client] [========                                ]  21%  615.2 KB / 2.9 MB  293.0 KB/s
[client] #3 canceled with 615.2 KB on disk (the .part file is kept, so running the same command again resumes)

$ scripts/run.sh client http://127.0.0.1:8733/slow.bin ./slow.bin
[client] #4 started: http://127.0.0.1:8733/slow.bin
[client]   total 2.9 MB, resuming at 615.2 KB (that part is not re-fetched)
[client] #4 complete: .../slow.bin
[client]   2.9 MB in 7.82s, 374.5 KB/s average
```

## 快速开始

前置条件只有两个：内核带 binder（见下文「关于内核」），以及装了 clang。

```bash
sudo apt update && sudo apt install --no-install-recommends clang cmake ninja-build

# 终端 A：构建 + 配置 binder + 拉起 servicemanager 和两个服务（Ctrl+C 退出）
scripts/run.sh

# 终端 B：跑客户端
scripts/run.sh client https://example.com/file.bin
scripts/run.sh client https://example.com/file.bin /tmp/out.bin
```

其它模式：

```bash
scripts/run.sh servicemanager   # 只在前台跑 servicemanager
scripts/run.sh http             # 只在前台跑 http service
scripts/run.sh download         # 只在前台跑 download service
```

`run.sh` 第一次会在 `build/`（可用 `BUILD_DIR` 覆盖）配置并构建，之后只做增量构建。如果 `/dev/binder` 还不存在，它会用 `sudo` 调 `scripts/setup-binder-host.sh` 把驱动准备好——**只有这一步需要 root**，服务和客户端都以普通用户身份运行。

### 手动构建

不想用脚本包一层的话：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

sudo scripts/setup-binder-host.sh   # 一次性配置 binder 驱动（重启后需重跑）

./build/service_manager                              # 注意：不需要 sudo
./build/download_client https://example.com/file.bin
```

`setup-binder-host.sh` 会加载 `binder_linux`、挂载 binderfs、创建 binder 设备，并把设备权限设为 `0666`——和 Android 上 `/dev/binder` 的权限一致。因此服务以普通用户身份即可运行，CLion 里直接点 Run 就行，不必以 root 启动 IDE。

撤销：

```bash
sudo scripts/setup-binder-host.sh --teardown
```

### 服务端入口

`service/main.cpp` 编译为 `service_manager`。不带参数运行时它是个启动器：检查 `/dev/binder`，拉起 `servicemanager`（如果还没有在跑），再把自己 fork/exec 两份分别去当两个服务，等它们都注册好之后守着它们，Ctrl+C 时统一收尾。

一个二进制三种角色是有意为之：启动一个角色就是带上参数 exec `/proc/self/exe`，除了 `servicemanager` 之外它不需要去别处找同伴二进制。

```
service_manager                 servicemanager + 两个服务（默认）
service_manager http            只跑 http service，前台
service_manager download        只跑 download service，前台
service_manager servicemanager  只跑 servicemanager
```

两个服务各占一个进程，而不是同一个进程里的两个对象——只有这样，download → http 的调用才是真正跨进程的 binder 事务，而不会被内核优化成一次本地函数调用。

### 两个服务

**`http.service`**（`service/http/`）提供 HTTP 请求能力，是整个项目里唯一知道 socket 是什么的地方。

* `HttpClient.{h,cpp}` 是一个不依赖 binder 的 HTTP/1.1 客户端：GET、重定向跟随、`Content-Length` 与 `chunked` 两种分帧、`Range` 断点续传、连接与空闲超时。构建时能找到 OpenSSL 就支持 HTTPS（含证书链和主机名校验）。响应体从不在内存里攒着，收到多少就交给回调多少。
* `HttpService.{h,cpp}` 把它放到 `IHttpService` 后面。`fetchToFd()` 写的是调用方传进来的 fd；`getString()` 是给小响应体准备的，超过 1 MiB 直接失败。
* 取消：`newRequestId()` 先拿一个 id，`cancel(id)` 是 oneway 的，传输线程在两次 socket 读之间看这个标志。

**`download.service`**（`service/download/`）提供下载能力，网络部分全部委托给 http service。它加的是文件这一侧的事情：传输期间写 `<目标路径>.part`，成功后原子改名；上次中断留下的 `.part` 就是断点，下次自动从它的长度处续传；`enqueue()` 立刻返回，每个任务跑在自己的工作线程上。

进度是三跳传回来的：http service 一边写 fd 一边 oneway 回调 download service，后者把「本次响应收了多少」换算成「这个下载一共有多少」、算出速度、再降频 oneway 转发给客户端。全链路 oneway，画进度条画得慢的客户端不会把下载拖慢。

### 客户端

`client/download.cpp` 编译为 `download_client`。它只链接 AIDL 生成的接口，不含任何服务端实现——客户端进程里既没有 HTTP 代码，也不知道 `http.service` 的存在。

```
download_client <url> [目标路径] [--cancel-after=<毫秒>] [--quiet]
```

目标路径省略时，文件名取自 URL，落在 `./downloads/` 下。相对路径由**客户端**换算成绝对路径之后再发出去——否则它会按 download service 那个进程的工作目录解析，文件就悄悄落到别处去了。

`--cancel-after` 用来演示取消：服务端会保留 `.part`，所以再跑一次同样的命令就是断点续传（写了 0 字节的失败不会留下空 `.part`）。

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

## binder 能力覆盖

每个服务的 `.aidl` 就放在自己的实现旁边（`service/http/aidl/`、`service/download/aidl/`），构建时由项目内置的 AIDL 编译器生成 C++，合成一个 `service_aidl` 库——客户端链接的也是它，所以接口只有一份定义，两边不可能对不上。AIDL 编译器要求文件路径和 `package` 对得上，所以这两个 aidl 根目录下还有一层 `com/service/<名字>/`。这套服务用到了：

| 能力 | 用在哪里 |
|---|---|
| 服务注册 / 发现 | 两个服务各自 `addService()`；`checkService()` + `interface_cast` 查找 |
| 服务调服务 | download service 是 http service 的客户端，中间那一跳是完整的 binder 事务 |
| 同步调用 | `enqueue()`、`fetchToFd()`、`getString()`，含 UTF-8 字符串、`long`、`int[]` |
| binder 引用传递 | 客户端把回调 binder 传给 download service；后者又把自己的回调 binder 传给 http service |
| oneway | 所有进度回调和 `cancel()`；发往同一 binder 对象的 oneway 事务按序投递 |
| fd 传递 | download service 把 `.part` 的 fd 通过 `ParcelFileDescriptor` 交给 http service，内核 dup 到对方进程 |
| 调用方身份 | `getCallingPid()` / `getCallingUid()`，由驱动提供而非报文携带 |
| 死亡通知 | 客户端对 download service 的代理 `linkToDeath()`；download service 用 `isBinderAlive()` 判断缓存的 http 代理是否失效 |
| service-specific 错误 | `Status::fromServiceSpecificError()` 把错误码和原因一路带回客户端 |

## 目录结构

```
service/                  服务端 -> service_manager
  main.cpp                入口：启动器 + 角色分发
  http/aidl/              IHttpService、IHttpProgressCallback
  http/HttpClient.*       不依赖 binder 的 HTTP/1.1 客户端（socket、TLS、chunked、Range）
  http/HttpService.*      IHttpService 实现
  download/aidl/          IDownloadService、IDownloadCallback
  download/DownloadService.*  IDownloadService 实现（.part、续传、进度中继）
client/download.cpp       客户端 -> download_client
CMakeLists.txt            顶层构建（所有库 + servicemanager）
external/android/         AOSP 源码，逐字节复制（见 PATCHES.md）
  frameworks/native/libs/binder/       libbinder
  frameworks/native/cmds/servicemanager/
  system/core/{libutils,libcutils,libsystem,libprocessgroup}/
  system/logging/liblog/  system/libbase/  external/fmtlib/
  bionic/libc/kernel/uapi/linux/android/   binder.h、binderfs.h
generated/aidl/           libbinder 自身的 AIDL，已预生成
compat/                   libselinux + libvintf 替代实现，以及 host 构建垫片
tools/bin/aidl            AOSP 预编译 AIDL 编译器（含 tools/lib64/libc++.so）
tools/binderfs_ctl.cpp    通过 BINDER_CTL_ADD 创建 binder 设备
patches/                  两个被修改的 AOSP 文件的 diff
scripts/                  run.sh（构建 + 运行）、setup-binder-host.sh（配置驱动）
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

因为整条 binder 链路就跑在宿主机上，断点调试不需要任何远程配置：`Reload CMake Project` 之后，Run 配置选 `service_manager` 即可。前提是先 `sudo scripts/setup-binder-host.sh` 配好驱动（重启后需重跑），之后不必以 root 启动 IDE。

`service_manager` 可以直接 Run：它发现没有 servicemanager 时会自己拉起一个（并在自己退出时把它一起收掉；如果 servicemanager 本来就在跑，则不动它），然后把两个服务也带起来。

**`download_client` 不能单独 Run**——它需要 `download.service` 已注册，所以要么先 Run `service_manager`，要么在另一个终端 `scripts/run.sh`。没有服务时它不会傻等，会直接报「`download.service` 没有注册」退出。

但连 `servicemanager` 都没有的话，会卡在这里：

```
ProcessState W  Not able to get context object on /dev/binder.
ServiceManager E  Waiting 1s on context object on /dev/binder.
```

这个循环**不会**超时退出：`defaultServiceManager()` 在 android-12 里是无限重试，不返回 null。`service_manager` 用的是同一次查找的非阻塞版本 `ProcessState::getContextObject()`——它 ping 一下 handle 0，没人占着就返回 null，所以才能拿它当探测手段。

注意 CLion 默认只构建被选中的那个 target，而 `service_manager` 在运行期需要 `servicemanager`（CMake 里已用 `add_dependencies` 声明，会一并构建）。`binderfs_ctl` 只被配置脚本用到，需要时单独构建：

```bash
cmake --build cmake-build-debug --target binderfs_ctl
```

所有可执行文件都输出到构建目录根下，`service_manager` 就是在「自身同目录」里找 `servicemanager` 的——这也正是 `cmake --install` 的 `bin/` 布局，两种情况用的是同一套查找逻辑。

## 重新生成 libbinder 的 AIDL

`generated/aidl/` 已经入库，构建时无需额外步骤，但可以重新生成：

```bash
B=external/android/frameworks/native/libs/binder
./tools/bin/aidl --lang=cpp -I $B/aidl \
  --header_out=generated/aidl --out=generated/aidl \
  $B/aidl/android/os/*.aidl $B/aidl/android/content/pm/*.aidl
```

`service/*/aidl/` 下的服务接口不需要手动生成，构建时会自动跑。

## 已知限制

* **SELinux 强制被桩掉了。** servicemanager 的按服务访问控制一律放行，详见 PATCHES.md。
* **VINTF 声明**来自文本文件，而非真正的 VINTF XML。
* **没有 logd。** liblog 以 host 模式构建，`ALOG*` 输出到 stderr。
* **Ashmem** 用的是 libcutils 的 host 实现（已 unlink 的临时文件），不是 `/dev/ashmem` 或 memfd。
* `libbinder` 的 RPC-over-sockets 代码编进去了，但这里没有用到。
* **http service 只做 GET。** 没有 POST、cookie、代理、连接复用，也不做内容解压（请求头写死 `Accept-Encoding: identity`）。构建时没有 OpenSSL 的话，`https://` 会带着明确的说明失败。
