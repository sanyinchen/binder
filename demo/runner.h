/*
 * The demo itself.
 *
 *   servicemanager   the context manager, binder handle 0
 *        |
 *   demo_service     registers "demo.service" with it
 *        |
 *   demo_client      looks the service up and exercises it
 */
#pragma once

namespace demo {

// Starts servicemanager and demo_service, runs demo_client against them, then
// tears the two down again. Returns the client's exit code.
int runAll();

// Runs a single companion binary in place of this process (exec, no return on
// success). `name` is the binary name, e.g. "demo_service".
int runOne(const char* name);

} // namespace demo
