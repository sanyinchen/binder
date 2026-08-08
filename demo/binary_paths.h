/*
 * Locating the companion binaries.
 *
 * binder_demo only fork/execs; the binder code lives in servicemanager,
 * demo_service and demo_client, which have to be found at runtime.
 */
#pragma once

#include <string>

namespace demo {

// Looks for `name` next to this executable (install tree), then in the places
// a build tree puts it -- examples/ below, or one level up from examples/ --
// then ../bin and /usr/local/bin. Returns "" if none of those hold an
// executable file of that name.
std::string findBinary(const std::string& name);

} // namespace demo
