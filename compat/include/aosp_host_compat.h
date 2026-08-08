/*
 * Force-included into every translation unit (see -include in CMakeLists.txt).
 *
 * AOSP builds against bionic, whose headers pull in a lot transitively.  Under
 * glibc + libstdc++ those same sources hit errors like "use of undeclared
 * identifier 'strerror'" or "no template named 'unique_ptr'" purely because the
 * include happened to come for free on Android.
 *
 * Providing the headers here keeps the copied AOSP sources byte-identical to
 * upstream instead of sprinkling #include fixups through them.
 */
#pragma once

#ifdef __cplusplus

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#else /* C */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#endif
