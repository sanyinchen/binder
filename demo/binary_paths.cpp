#include "binary_paths.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace demo {
namespace {

bool isExecutable(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string selfDir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    const char* slash = strrchr(buf, '/');
    return slash ? std::string(buf, slash - buf) : std::string(".");
}

} // namespace

std::string findBinary(const std::string& name) {
    const std::string dir = selfDir();
    const std::vector<std::string> candidates = {
            dir + "/" + name,          // install tree, or same dir in a build tree
            dir + "/examples/" + name, // binder_demo at the build root -> examples/
            dir + "/../" + name,       // demo_service in examples/ -> the build root
            dir + "/../bin/" + name,
            "/usr/local/bin/" + name,
    };
    for (const auto& path : candidates) {
        if (!isExecutable(path)) continue;
        // Collapse the ../ in the build-tree candidates so log lines are readable.
        char* resolved = realpath(path.c_str(), nullptr);
        if (resolved == nullptr) return path;
        std::string canonical(resolved);
        free(resolved);
        return canonical;
    }
    return "";
}

} // namespace demo
