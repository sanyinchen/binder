/* Minimal libvintf replacement — see include/vintf/VintfObject.h. */

#include <vintf/VintfObject.h>
#include <vintf/constants.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace android {
namespace vintf {

namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// "some.package.IFoo/default" or "some.package.IFoo/default@com.some.apex"
bool parseLine(const std::string& line, ManifestInstance* out) {
    const auto slash = line.find('/');
    if (slash == std::string::npos) return false;

    std::string fqName = line.substr(0, slash);
    std::string rest = line.substr(slash + 1);

    std::optional<std::string> apex;
    const auto at = rest.find('@');
    if (at != std::string::npos) {
        apex = rest.substr(at + 1);
        rest = rest.substr(0, at);
    }

    const auto lastDot = fqName.rfind('.');
    if (lastDot == std::string::npos) return false;

    *out = ManifestInstance(fqName.substr(0, lastDot), fqName.substr(lastDot + 1), rest,
                            std::move(apex));
    return true;
}

} // namespace

std::shared_ptr<const HalManifest> HalManifest::loadFrom(const std::string& path) {
    auto manifest = std::make_shared<HalManifest>();

    std::ifstream in(path);
    if (!in) return manifest; // absent file == empty manifest

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        ManifestInstance instance("", "", "", std::nullopt);
        if (parseLine(line, &instance)) {
            manifest->add(std::move(instance));
        }
    }
    return manifest;
}

bool HalManifest::hasAidlInstance(const std::string& package, const std::string& interface,
                                  const std::string& instance) const {
    for (const auto& mi : mInstances) {
        if (mi.package() == package && mi.interface() == interface && mi.instance() == instance) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> HalManifest::getAidlInstances(const std::string& package,
                                                       const std::string& interface) const {
    std::vector<std::string> ret;
    for (const auto& mi : mInstances) {
        if (mi.package() == package && mi.interface() == interface) {
            ret.push_back(mi.instance());
        }
    }
    return ret;
}

void HalManifest::forEachInstance(const std::function<bool(const ManifestInstance&)>& func) const {
    for (const auto& mi : mInstances) {
        if (func(mi)) return; // true == stop
    }
}

std::shared_ptr<const HalManifest> VintfObject::GetDeviceHalManifest() {
    static const std::shared_ptr<const HalManifest> manifest = [] {
        const char* override_path = getenv(kVintfManifestPathEnv);
        return HalManifest::loadFrom(override_path ? override_path : kDeviceManifestPath);
    }();
    return manifest;
}

std::shared_ptr<const HalManifest> VintfObject::GetFrameworkHalManifest() {
    static const std::shared_ptr<const HalManifest> manifest =
            HalManifest::loadFrom(kFrameworkManifestPath);
    return manifest;
}

} // namespace vintf
} // namespace android
