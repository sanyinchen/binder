/*
 * Thin fork/exec/wait helpers.
 *
 * Children inherit stdout/stderr on purpose, so service-side and client-side
 * output interleave live -- that is the interesting part of watching IPC.
 */
#pragma once

#include <string>
#include <sys/types.h>

namespace demo {

// fork + exec `path` with no arguments. Returns the child pid, or -1 if the
// fork failed; an exec failure shows up as a child that exits with 127.
pid_t spawn(const std::string& path);

// False if the child has already exited, i.e. it failed at startup. Reaps it
// in that case, so the caller must not wait on it again.
bool stillAlive(pid_t pid);

// Blocks until the child exits; returns its exit code, or 1 if it was killed
// by a signal.
int waitForExit(pid_t pid);

// SIGTERM + wait. Sets `pid` to -1, so calling it twice is harmless.
void terminate(pid_t& pid);

// Replaces the current process with `path`. Only returns on failure, with 127.
int execReplace(const std::string& path);

} // namespace demo
