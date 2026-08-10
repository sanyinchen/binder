/*
 * A small HTTP/1.1 client: sockets, request framing, response parsing.
 *
 * This is the only part of the project that knows what a socket is; it has no
 * binder dependency at all, so it can be reasoned about (and reused) on its
 * own. HttpService.cpp is the layer that puts it behind a binder interface.
 *
 * Scope, deliberately: GET only, `Connection: close` (no pooling), no cookies,
 * no compression, no proxy. HTTPS is available when the project is built
 * against OpenSSL -- see tlsSupported().
 *
 * The body is never accumulated: it is handed to a callback as it arrives, so
 * memory use is one buffer regardless of response size.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace httpsvc {

// -------------------------------------------------------------------------
// Failure codes. These are the IHttpService.ERROR_* values -- the transport
// speaks the same vocabulary as the binder interface so nothing has to be
// translated on the way out. Kept as an enum rather than including the
// generated AIDL header, so this file stays binder-free.
// -------------------------------------------------------------------------
enum Error {
    kOk = 0,
    kErrorBadUrl = -1,
    kErrorConnect = -2,
    kErrorIo = -3,
    kErrorHttpStatus = -4,
    kErrorCanceled = -5,
    kErrorWrite = -6,
    kErrorTooLarge = -7,
};

struct Url {
    std::string scheme; // "http" or "https", lowercased
    std::string host;   // no brackets, even for IPv6 literals
    std::string port;   // decimal; defaulted from the scheme when absent
    std::string target; // path + query, always starting with '/'

    // Host header value: host, plus ":port" when it is not the scheme default.
    std::string hostHeader() const;
    std::string toString() const;
};

bool parseUrl(const std::string& text, Url* out);

// Resolves a Location header against the URL it was received from. Handles
// absolute URLs, absolute paths and relative paths. Empty on failure.
std::string resolveRedirect(const Url& base, const std::string& location);

// True when this build can speak https. When false, https:// URLs fail with
// kErrorBadUrl and a message saying so.
bool tlsSupported();

struct ResponseInfo {
    int statusCode = 0;
    std::string reason;
    std::string contentType;
    // Where the returned body belongs in the target file: `resumeFrom` when the
    // server honoured the Range request (206), 0 when it ignored it (200).
    int64_t startOffset = 0;
    // Size of the complete resource, -1 when the server did not say.
    int64_t totalBytes = -1;
    // URL the body actually came from, after redirects.
    std::string finalUrl;
};

struct Request {
    std::string url;
    // Sent as `Range: bytes=N-`. The server is free to ignore it; check
    // ResponseInfo::startOffset to see whether it did.
    int64_t resumeFrom = 0;
    int maxRedirects = 5;
    int connectTimeoutMs = 15000;
    // Time without a single byte arriving before the transfer is given up on.
    int idleTimeoutMs = 30000;
};

struct Result {
    int error = kOk;
    std::string message; // human-readable; empty on success
    ResponseInfo response;
    int64_t bytes = 0; // body bytes passed to the sink
};

// Called once, after the status line and headers, before any body byte.
// Returning false aborts the transfer with kErrorWrite.
using HeadersFn = std::function<bool(const ResponseInfo&)>;

// Body bytes in arrival order. Returning false aborts with kErrorWrite; use it
// for a failing sink, not for cancellation.
using BodyFn = std::function<bool(const char* data, size_t len)>;

// Polled between socket reads. Returning true aborts with kErrorCanceled.
// May be empty.
using CancelFn = std::function<bool()>;

// Performs the GET, following redirects. Blocks until the body is complete,
// the transfer fails, or `isCanceled` says to stop.
Result get(const Request& request, const HeadersFn& onHeaders, const BodyFn& onBody,
           const CancelFn& isCanceled);

// Convenience wrapper collecting the body in memory. Fails with kErrorTooLarge
// past `maxBytes`.
Result getString(const std::string& url, size_t maxBytes, std::string* body);

} // namespace httpsvc
