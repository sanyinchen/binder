cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Toolchain: clang is required.
#
# AOSP builds libbinder with clang only (its Android.bp sets `clang: true`), and
# the sources rely on clang-specific tolerances that GCC rejects outright:
#   * utils/Vector.h + SortedVector.h declare `const operator=` overloads whose
#     bodies call the non-const VectorImpl::operator=. Clang defers the error to
#     instantiation (and nothing ever instantiates them); GCC diagnoses at parse.
#   * binder/Parcel.h puts __attribute__((deprecated(...))) between the parameter
#     list and the body on ~9 function *definitions*. GCC only accepts attributes
#     on declarations.
#
# Pick clang up automatically so IDEs work without extra configuration, unless
# the caller has already chosen a compiler.
# ---------------------------------------------------------------------------
# Look for the unversioned name first, then versioned ones (newest first) --
# Debian/Ubuntu only create /usr/bin/clang when the `clang` metapackage is
# installed, so a system with just `clang-18` has no unversioned symlink.
#
# Each candidate is actually executed before being accepted. A machine with
# LLVM installed from both the distro and apt.llvm.org can end up with a clang
# that loads two different libLLVM copies and dies at startup with
# "Option ... registered more than once"; such a compiler must be skipped, not
# baked into the cache.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  foreach(_suffix "" "-21" "-20" "-19" "-18" "-17" "-16" "-15" "-14")
    find_program(_cand_c   NAMES "clang${_suffix}")
    find_program(_cand_cxx NAMES "clang++${_suffix}")
    if(_cand_c AND _cand_cxx)
      execute_process(COMMAND "${_cand_cxx}" --version
                      RESULT_VARIABLE _rc
                      OUTPUT_QUIET ERROR_QUIET)
      if(_rc EQUAL 0)
        set(CMAKE_C_COMPILER   "${_cand_c}"   CACHE FILEPATH "C compiler")
        set(CMAKE_CXX_COMPILER "${_cand_cxx}" CACHE FILEPATH "C++ compiler")
        message(STATUS "Selected clang toolchain: ${_cand_cxx}")
        break()
      else()
        message(STATUS "Skipping broken clang: ${_cand_cxx} (exit ${_rc})")
      endif()
    endif()
    unset(_cand_c CACHE)
    unset(_cand_cxx CACHE)
  endforeach()
  unset(_cand_c CACHE)
  unset(_cand_cxx CACHE)
endif()

project(android_binder LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_msg "clang is required, but CMake selected ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).")
  string(APPEND _msg [[

The AOSP sources under external/android/ do not compile with GCC; see the
toolchain comment at the top of this file for the exact constructs involved.

How to fix, pick one:

  1. Install clang, then configure a FRESH build directory (clang is then
     picked up automatically):
         sudo apt update && sudo apt install --no-install-recommends clang
         rm -rf cmake-build-debug
     In CLion: Settings > Build, Execution, Deployment > Toolchains, set
     C Compiler to clang and C++ Compiler to clang++, then
     File > Reload CMake Project.

  2. Select it explicitly:
         cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

  3. Let scripts/run.sh configure the build for you; it pins clang.
]])
  message(FATAL_ERROR "${_msg}")
endif()

# ---------------------------------------------------------------------------
# Android 12 (AOSP) binder IPC stack, built for glibc/Linux with CMake.
#
# Sources under external/android/ are copied verbatim from the AOSP android-12
# tree.  Everything needed to build and run lives inside this project; nothing
# references the original Android source tree at build time.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(AOSP "${CMAKE_CURRENT_SOURCE_DIR}/external/android")
set(GEN  "${CMAKE_CURRENT_SOURCE_DIR}/generated/aidl")
set(COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/compat")

# AOSP code is compiled with clang and is not warning-clean under gcc's or a
# newer clang's default set.  Silence the noise rather than patching upstream.
add_compile_options(
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-deprecated-declarations
  -Wno-unknown-pragmas
  -Wno-attributes
)

# Supplies the standard headers AOSP gets transitively from bionic, plus a
# libc++-compatible <stdatomic.h> that must shadow clang's C11 one.
add_compile_options(-include ${COMPAT}/include/aosp_host_compat.h)
include_directories(BEFORE ${COMPAT}/include)

find_package(Threads REQUIRED)

# ===========================================================================
# fmtlib  (libbase dependency)
#
# Header-only: this fmt release's src/format.cc explicitly instantiates
# detail::basic_data<void>, which current clang rejects as a duplicate of the
# instantiation in format-inl.h.  FMT_HEADER_ONLY sidesteps it without touching
# the vendored source.
# ===========================================================================
add_library(fmtlib INTERFACE)
target_include_directories(fmtlib INTERFACE ${AOSP}/external/fmtlib/include)
target_compile_definitions(fmtlib INTERFACE FMT_HEADER_ONLY=1)

# ===========================================================================
# liblog  (host variant: log output goes to stderr)
# ===========================================================================
add_library(log STATIC
  ${AOSP}/system/logging/liblog/log_event_list.cpp
  ${AOSP}/system/logging/liblog/log_event_write.cpp
  ${AOSP}/system/logging/liblog/logger_name.cpp
  ${AOSP}/system/logging/liblog/logger_read.cpp
  ${AOSP}/system/logging/liblog/logger_write.cpp
  ${AOSP}/system/logging/liblog/logprint.cpp
  ${AOSP}/system/logging/liblog/properties.cpp
  ${AOSP}/system/logging/liblog/event_tag_map.cpp
)
target_include_directories(log
  PUBLIC  ${AOSP}/system/logging/liblog/include
          ${AOSP}/system/libbase/include
          ${AOSP}/system/core/libcutils/include
  PRIVATE ${AOSP}/system/logging/liblog
          ${AOSP}/system/core/libutils/include   # <utils/FastStrcmp.h>
          ${AOSP}/system/core/libsystem/include  # pulled in via utils/ThreadDefs.h
)
target_link_libraries(log PUBLIC Threads::Threads)

# ===========================================================================
# libcutils  (host variant)
# ===========================================================================
add_library(cutils STATIC
  ${AOSP}/system/core/libcutils/config_utils.cpp
  ${AOSP}/system/core/libcutils/canned_fs_config.cpp
  ${AOSP}/system/core/libcutils/iosched_policy.cpp
  ${AOSP}/system/core/libcutils/load_file.cpp
  ${AOSP}/system/core/libcutils/native_handle.cpp
  ${AOSP}/system/core/libcutils/multiuser.cpp   # AOSP marks this android-only; it is pure arithmetic
  ${AOSP}/system/core/libcutils/properties.cpp
  ${AOSP}/system/core/libcutils/record_stream.cpp
  ${AOSP}/system/core/libcutils/strlcpy.c
  ${AOSP}/system/core/libcutils/threads.cpp
  ${AOSP}/system/core/libcutils/sockets.cpp
  ${AOSP}/system/core/libcutils/socket_inaddr_any_server_unix.cpp
  ${AOSP}/system/core/libcutils/socket_local_client_unix.cpp
  ${AOSP}/system/core/libcutils/socket_local_server_unix.cpp
  ${AOSP}/system/core/libcutils/socket_network_client_unix.cpp
  ${AOSP}/system/core/libcutils/sockets_unix.cpp
  ${AOSP}/system/core/libcutils/ashmem-host.cpp
  ${AOSP}/system/core/libcutils/fs_config.cpp
  ${AOSP}/system/core/libcutils/trace-host.cpp
)
target_include_directories(cutils
  PUBLIC  ${AOSP}/system/core/libcutils/include
          ${AOSP}/system/core/libcutils/include_outside_system
          ${AOSP}/system/core/libutils/include
          ${AOSP}/system/logging/liblog/include
  PRIVATE ${AOSP}/system/core/libcutils
)
target_link_libraries(cutils PUBLIC log Threads::Threads)

# ===========================================================================
# libutils
# ===========================================================================
add_library(utils STATIC
  ${AOSP}/system/core/libutils/Errors.cpp
  ${AOSP}/system/core/libutils/FileMap.cpp
  ${AOSP}/system/core/libutils/JenkinsHash.cpp
  ${AOSP}/system/core/libutils/LightRefBase.cpp
  ${AOSP}/system/core/libutils/NativeHandle.cpp
  ${AOSP}/system/core/libutils/Printer.cpp
  ${AOSP}/system/core/libutils/RefBase.cpp
  ${AOSP}/system/core/libutils/SharedBuffer.cpp
  ${AOSP}/system/core/libutils/StopWatch.cpp
  ${AOSP}/system/core/libutils/String8.cpp
  ${AOSP}/system/core/libutils/String16.cpp
  ${AOSP}/system/core/libutils/StrongPointer.cpp
  ${AOSP}/system/core/libutils/SystemClock.cpp
  ${AOSP}/system/core/libutils/Threads.cpp
  ${AOSP}/system/core/libutils/Timers.cpp
  ${AOSP}/system/core/libutils/Tokenizer.cpp
  ${AOSP}/system/core/libutils/Unicode.cpp
  ${AOSP}/system/core/libutils/VectorImpl.cpp
  ${AOSP}/system/core/libutils/misc.cpp
  ${AOSP}/system/core/libutils/Looper.cpp
)
target_include_directories(utils
  PUBLIC ${AOSP}/system/core/libutils/include
         ${AOSP}/system/core/libcutils/include
         ${AOSP}/system/core/libsystem/include        # <system/graphics.h>, thread_defs.h
         ${AOSP}/system/unwinding/libbacktrace/include # <backtrace/backtrace_constants.h>
         ${AOSP}/system/logging/liblog/include
)
target_link_libraries(utils PUBLIC log cutils Threads::Threads)

# ===========================================================================
# libbase
# ===========================================================================
add_library(base STATIC
  ${AOSP}/system/libbase/abi_compatibility.cpp
  ${AOSP}/system/libbase/chrono_utils.cpp
  ${AOSP}/system/libbase/cmsg.cpp
  ${AOSP}/system/libbase/file.cpp
  ${AOSP}/system/libbase/logging.cpp
  ${AOSP}/system/libbase/mapped_file.cpp
  ${AOSP}/system/libbase/parsebool.cpp
  ${AOSP}/system/libbase/parsenetaddress.cpp
  ${AOSP}/system/libbase/process.cpp
  ${AOSP}/system/libbase/properties.cpp
  ${AOSP}/system/libbase/stringprintf.cpp
  ${AOSP}/system/libbase/strings.cpp
  ${AOSP}/system/libbase/threads.cpp
  ${AOSP}/system/libbase/errors_unix.cpp
)
target_include_directories(base
  PUBLIC ${AOSP}/system/libbase/include
         ${AOSP}/system/logging/liblog/include
)
target_link_libraries(base PUBLIC log fmtlib Threads::Threads)

# ===========================================================================
# libbinder
#
# BINDER_ENABLE_KERNEL_IPC turns on the /dev/binder ioctl paths that upstream
# guards behind __ANDROID__ (see PATCHES.md).  Without it libbinder builds in
# "host" mode where every transaction returns INVALID_OPERATION.
# ===========================================================================
add_library(binder STATIC
  ${AOSP}/frameworks/native/libs/binder/Binder.cpp
  ${AOSP}/frameworks/native/libs/binder/BpBinder.cpp
  ${AOSP}/frameworks/native/libs/binder/BufferedTextOutput.cpp
  ${AOSP}/frameworks/native/libs/binder/Debug.cpp
  ${AOSP}/frameworks/native/libs/binder/IInterface.cpp
  ${AOSP}/frameworks/native/libs/binder/IMemory.cpp
  ${AOSP}/frameworks/native/libs/binder/IPCThreadState.cpp
  ${AOSP}/frameworks/native/libs/binder/IResultReceiver.cpp
  ${AOSP}/frameworks/native/libs/binder/IServiceManager.cpp
  ${AOSP}/frameworks/native/libs/binder/IShellCallback.cpp
  ${AOSP}/frameworks/native/libs/binder/LazyServiceRegistrar.cpp
  ${AOSP}/frameworks/native/libs/binder/MemoryBase.cpp
  ${AOSP}/frameworks/native/libs/binder/MemoryDealer.cpp
  ${AOSP}/frameworks/native/libs/binder/MemoryHeapBase.cpp
  ${AOSP}/frameworks/native/libs/binder/Parcel.cpp
  ${AOSP}/frameworks/native/libs/binder/ParcelableHolder.cpp
  ${AOSP}/frameworks/native/libs/binder/ParcelFileDescriptor.cpp
  ${AOSP}/frameworks/native/libs/binder/PersistableBundle.cpp
  ${AOSP}/frameworks/native/libs/binder/ProcessState.cpp
  ${AOSP}/frameworks/native/libs/binder/RpcAddress.cpp
  ${AOSP}/frameworks/native/libs/binder/RpcSession.cpp
  ${AOSP}/frameworks/native/libs/binder/RpcServer.cpp
  ${AOSP}/frameworks/native/libs/binder/RpcState.cpp
  ${AOSP}/frameworks/native/libs/binder/Static.cpp
  ${AOSP}/frameworks/native/libs/binder/Stability.cpp
  ${AOSP}/frameworks/native/libs/binder/Status.cpp
  ${AOSP}/frameworks/native/libs/binder/TextOutput.cpp
  ${AOSP}/frameworks/native/libs/binder/Utils.cpp
  # AIDL-generated
  ${GEN}/android/os/IClientCallback.cpp
  ${GEN}/android/os/IServiceCallback.cpp
  ${GEN}/android/os/IServiceManager.cpp
  ${GEN}/android/os/ServiceDebugInfo.cpp
  ${GEN}/android/content/pm/IPackageChangeObserver.cpp
  ${GEN}/android/content/pm/IPackageManagerNative.cpp
  ${GEN}/android/content/pm/PackageChangeEvent.cpp
)
target_include_directories(binder
  PUBLIC  ${AOSP}/frameworks/native/libs/binder/include
          ${AOSP}/frameworks/native/libs/binder/ndk/include_cpp  # binder_to_string.h
          ${AOSP}/system/core/libprocessgroup/include            # processgroup/sched_policy.h
          ${GEN}
          ${AOSP}/bionic/libc/kernel/uapi          # <linux/android/binder.h>
  PRIVATE ${AOSP}/frameworks/native/libs/binder    # Static.h, binder_module.h
)
target_compile_definitions(binder
  PUBLIC  BINDER_ENABLE_KERNEL_IPC
          ANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION
          ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION
)
target_link_libraries(binder PUBLIC utils cutils log base Threads::Threads ${CMAKE_DL_LIBS})

# ===========================================================================
# Host compatibility shims: libselinux / libvintf
# ===========================================================================
add_subdirectory(compat)

# ===========================================================================
# servicemanager
# ===========================================================================
add_executable(servicemanager
  ${AOSP}/frameworks/native/cmds/servicemanager/Access.cpp
  ${AOSP}/frameworks/native/cmds/servicemanager/ServiceManager.cpp
  ${AOSP}/frameworks/native/cmds/servicemanager/main.cpp
)
target_include_directories(servicemanager
  PRIVATE ${AOSP}/frameworks/native/cmds/servicemanager
)
target_link_libraries(servicemanager PRIVATE binder utils cutils log base selinux_stub vintf_stub)

# ===========================================================================
# binderfs_ctl -- creates binder devices via BINDER_CTL_ADD (see the source)
# ===========================================================================
add_executable(binderfs_ctl tools/binderfs_ctl.cpp)
target_include_directories(binderfs_ctl PRIVATE ${AOSP}/bionic/libc/kernel/uapi)

# ===========================================================================
# Examples
# ===========================================================================
add_subdirectory(examples)

# ===========================================================================
# demo_support -- finding the companion binaries and fork/exec/wait.
#
# Shared by binder_demo and demo_service (which starts servicemanager itself
# when none is running). No binder dependency: it is plain POSIX.
# ===========================================================================
add_library(demo_support STATIC
  demo/binary_paths.cpp
  demo/child_process.cpp
)
target_include_directories(demo_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ===========================================================================
# binder_demo -- the demo entry point (main.cpp + demo/)
#
# Spawns servicemanager and demo_service, then runs demo_client against them.
# It only fork/execs, so it links no binder code; the dependency on the other
# targets is just build order.
# ===========================================================================
add_executable(binder_demo main.cpp demo/runner.cpp)
target_link_libraries(binder_demo PRIVATE demo_support)
add_dependencies(binder_demo servicemanager demo_service demo_client)

# ===========================================================================
# Install layout for `cmake --install` (optional -- the demo also runs straight
# out of the build tree)
# ===========================================================================
install(TARGETS servicemanager binderfs_ctl RUNTIME DESTINATION bin)
install(TARGETS demo_service demo_client RUNTIME DESTINATION bin)
install(TARGETS binder_demo RUNTIME DESTINATION bin)
