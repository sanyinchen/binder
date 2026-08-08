/*
 * Minimal libvintf replacement for the Docker runtime.
 *
 * Real libvintf parses device/framework VINTF XML manifests and pulls in
 * tinyxml2, libhidlmetadata, libfs_mgr and friends.  servicemanager only needs
 * to answer three questions about AIDL instances, so this provides exactly that
 * API backed by a plain text manifest instead of XML.
 *
 * Manifest file (optional), one entry per line, blank lines and '#' comments
 * ignored:
 *
 *     some.package.IFoo/default
 *     some.package.IBar/instance@com.android.some.apex
 *
 * Locations, in order of precedence:
 *   $BINDER_VINTF_MANIFEST
 *   /etc/binder/vintf_manifest.txt        (device manifest)
 *   /etc/binder/vintf_manifest_fwk.txt    (framework manifest)
 *
 * With no manifest present every instance is simply undeclared, which is the
 * correct answer for a system that ships no VINTF HALs.  Only binders marked
 * with vendor stability consult this at all.
 */
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace android {
namespace vintf {

enum class HalFormat {
    HIDL = 0,
    NATIVE = 1,
    AIDL = 2,
};

class ManifestInstance {
public:
    ManifestInstance(std::string package, std::string interface, std::string instance,
                     std::optional<std::string> updatableViaApex)
          : mPackage(std::move(package)),
            mInterface(std::move(interface)),
            mInstance(std::move(instance)),
            mUpdatableViaApex(std::move(updatableViaApex)) {}

    HalFormat format() const { return HalFormat::AIDL; }
    const std::string& package() const { return mPackage; }
    const std::string& interface() const { return mInterface; }
    const std::string& instance() const { return mInstance; }
    std::optional<std::string> updatableViaApex() const { return mUpdatableViaApex; }

private:
    std::string mPackage;
    std::string mInterface;
    std::string mInstance;
    std::optional<std::string> mUpdatableViaApex;
};

class HalManifest {
public:
    // Parses `path`; a missing file yields an empty manifest.
    static std::shared_ptr<const HalManifest> loadFrom(const std::string& path);

    bool hasAidlInstance(const std::string& package, const std::string& interface,
                         const std::string& instance) const;

    std::vector<std::string> getAidlInstances(const std::string& package,
                                              const std::string& interface) const;

    // func returns true to stop iterating (libvintf's convention).
    void forEachInstance(const std::function<bool(const ManifestInstance&)>& func) const;

    void add(ManifestInstance instance) { mInstances.push_back(std::move(instance)); }

private:
    std::vector<ManifestInstance> mInstances;
};

class VintfObject {
public:
    static std::shared_ptr<const HalManifest> GetDeviceHalManifest();
    static std::shared_ptr<const HalManifest> GetFrameworkHalManifest();
};

} // namespace vintf
} // namespace android
