/* Minimal libvintf replacement — see VintfObject.h. */
#pragma once

#include <cstddef>

namespace android {
namespace vintf {

constexpr size_t kDefaultAidlVersion = 1;
constexpr char kVintfManifestPathEnv[] = "BINDER_VINTF_MANIFEST";
constexpr char kDeviceManifestPath[] = "/etc/binder/vintf_manifest.txt";
constexpr char kFrameworkManifestPath[] = "/etc/binder/vintf_manifest_fwk.txt";

} // namespace vintf
} // namespace android
