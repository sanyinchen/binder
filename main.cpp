/*
 * Demo entry point -- argument dispatch only; the work lives in demo/.
 *
 *   demo/binary_paths   finding servicemanager / demo_service / demo_client
 *   demo/child_process  fork, exec, wait
 *   demo/runner         the demo itself
 *
 * Usage:
 *   binder_demo              run servicemanager + service + client (default)
 *   binder_demo service      run only the demo service (foreground)
 *   binder_demo client       run only the demo client (foreground)
 *   binder_demo servicemanager
 *
 * The binder driver must already be set up -- scripts/run.sh does that via
 * scripts/setup-binder-host.sh; see README.md.
 */

#include "demo/runner.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "demo";

    if (strcmp(mode, "demo") == 0 || strcmp(mode, "all") == 0) return demo::runAll();
    if (strcmp(mode, "service") == 0) return demo::runOne("demo_service");
    if (strcmp(mode, "client") == 0) return demo::runOne("demo_client");
    if (strcmp(mode, "servicemanager") == 0) return demo::runOne("servicemanager");

    fprintf(stderr, "usage: %s [demo|service|client|servicemanager]\n", argv[0]);
    return 2;
}
