/*
 * binderfs_ctl -- create a binder device inside a mounted binderfs.
 *
 * The host kernel is built with CONFIG_ANDROID_BINDER_DEVICES="", so mounting
 * binderfs yields only the "binder-control" node.  Individual devices are
 * created with the BINDER_CTL_ADD ioctl, which no standard userspace tool
 * exposes -- hence this helper.
 *
 *   usage: binderfs_ctl <binderfs-mount-point> <device-name>...
 *   e.g.   binderfs_ctl /dev/binderfs binder hwbinder vndbinder
 *
 * Prints the major:minor of each device created.  A device that already exists
 * is reported and not treated as an error.
 */

#include <linux/android/binderfs.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <binderfs-mount-point> <device-name>...\n", argv[0]);
        return 2;
    }

    const std::string mountPoint = argv[1];
    const std::string control = mountPoint + "/binder-control";

    const int ctlFd = open(control.c_str(), O_RDONLY | O_CLOEXEC);
    if (ctlFd < 0) {
        fprintf(stderr, "binderfs_ctl: open(%s): %s\n", control.c_str(), strerror(errno));
        fprintf(stderr, "  is binderfs mounted there? (mount -t binder binder %s)\n",
                mountPoint.c_str());
        return 1;
    }

    int failures = 0;
    for (int i = 2; i < argc; i++) {
        const char* name = argv[i];
        if (strlen(name) > BINDERFS_MAX_NAME) {
            fprintf(stderr, "binderfs_ctl: name too long: %s\n", name);
            failures++;
            continue;
        }

        struct binderfs_device device = {};
        strncpy(device.name, name, BINDERFS_MAX_NAME);

        if (ioctl(ctlFd, BINDER_CTL_ADD, &device) < 0) {
            if (errno == EEXIST) {
                printf("binderfs_ctl: %s/%s already exists\n", mountPoint.c_str(), name);
                continue;
            }
            fprintf(stderr, "binderfs_ctl: BINDER_CTL_ADD(%s): %s\n", name, strerror(errno));
            failures++;
            continue;
        }

        printf("binderfs_ctl: created %s/%s (%u:%u)\n", mountPoint.c_str(), name, device.major,
               device.minor);
    }

    close(ctlFd);
    return failures == 0 ? 0 : 1;
}
