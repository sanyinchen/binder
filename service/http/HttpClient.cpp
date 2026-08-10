#include "HttpClient.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HTTP_SERVICE_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace httpsvc {
namespace {

constexpr char kUserAgent[] = "binder-http-service/1.0";
constexpr size_t kReadBufferSize = 64 * 1024;
constexpr size_t kMaxLineLength = 8 * 1024; // status line, header line, chunk header
constexpr size_t kMaxHeaderCount = 100;
// How long a single socket read waits before coming back empty-handed. It is
// not a transfer timeout: it is how often the cancel flag gets looked at while
// a transfer is stalled.
constexpr int kPollSliceMs = 250;

using Clock = std::chrono::steady_clock;

int64_t elapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string describeErrno(const char* what) {
    return std::string(what) + ": " + strerror(errno);
}

// -------------------------------------------------------------------------
// Header bag
//
// HTTP header names are case-insensitive, and a response may repeat one. Only
// the first value of each is ever needed here, so lookup keeps it simple.
// -------------------------------------------------------------------------
class Headers {
public:
    void add(const std::string& name, const std::string& value) {
        mItems.emplace_back(toLower(name), value);
    }

    std::string get(const std::string& name) const {
        const std::string key = toLower(name);
        for (const auto& [n, v] : mItems) {
            if (n == key) return v;
        }
        return {};
    }

    bool contains(const std::string& name, const std::string& needle) const {
        return toLower(get(name)).find(toLower(needle)) != std::string::npos;
    }

    size_t size() const { return mItems.size(); }

private:
    std::vector<std::pair<std::string, std::string>> mItems;
};

// -------------------------------------------------------------------------
// Connections
//
// One interface over a plain socket and a TLS session, so the HTTP layer above
// never branches on the scheme. recv() reports "nothing yet, ask again" as a
// distinct result (kAgain) instead of an error: that is the point at which the
// cancel flag and the idle timeout get checked.
// -------------------------------------------------------------------------
constexpr ssize_t kAgain = -2;

class Conn {
public:
    virtual ~Conn() = default;
    // >0: bytes read. 0: peer closed. kAgain: retry. -1: broken.
    virtual ssize_t recv(char* buf, size_t len) = 0;
    virtual bool sendAll(const char* buf, size_t len) = 0;
    const std::string& error() const { return mError; }

protected:
    std::string mError;
};

class PlainConn : public Conn {
public:
    explicit PlainConn(int fd) : mFd(fd) {}
    ~PlainConn() override {
        if (mFd >= 0) close(mFd);
    }

    ssize_t recv(char* buf, size_t len) override {
        const ssize_t n = ::read(mFd, buf, len);
        if (n >= 0) return n;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return kAgain;
        mError = describeErrno("read");
        return -1;
    }

    bool sendAll(const char* buf, size_t len) override {
        while (len > 0) {
            const ssize_t n = ::write(mFd, buf, len);
            if (n > 0) {
                buf += n;
                len -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            mError = describeErrno("write");
            return false;
        }
        return true;
    }

protected:
    int mFd;
};

#ifdef HTTP_SERVICE_HAVE_TLS

// Process-wide, built once: loading the system trust store on every request
// would dominate the cost of a small download.
SSL_CTX* tlsContext() {
    static SSL_CTX* ctx = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (c == nullptr) return static_cast<SSL_CTX*>(nullptr);
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(c) != 1) {
            // No trust store: verification would reject everything, so it is
            // better to fail the handshake with a clear message than to keep a
            // context that cannot work.
            SSL_CTX_free(c);
            return static_cast<SSL_CTX*>(nullptr);
        }
        return c;
    }();
    return ctx;
}

std::string tlsError(const char* what) {
    const unsigned long code = ERR_get_error();
    char buf[256] = {0};
    if (code != 0) ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(what) + (code != 0 ? std::string(": ") + buf : std::string());
}

class TlsConn : public PlainConn {
public:
    TlsConn(int fd, SSL* ssl) : PlainConn(fd), mSsl(ssl) {}
    ~TlsConn() override {
        if (mSsl != nullptr) {
            SSL_shutdown(mSsl);
            SSL_free(mSsl);
        }
    }

    ssize_t recv(char* buf, size_t len) override {
        ERR_clear_error();
        const int n = SSL_read(mSsl, buf, static_cast<int>(len));
        if (n > 0) return n;
        switch (SSL_get_error(mSsl, n)) {
            case SSL_ERROR_ZERO_RETURN:
                return 0;
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                return kAgain;
            case SSL_ERROR_SYSCALL:
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return kAgain;
                // A server that drops the TCP connection without close_notify
                // is common enough that it must not be reported as an error;
                // the framing (Content-Length / chunked) catches truncation.
                if (errno == 0) return 0;
                mError = describeErrno("tls read");
                return -1;
            default:
                mError = tlsError("tls read");
                return -1;
        }
    }

    bool sendAll(const char* buf, size_t len) override {
        while (len > 0) {
            ERR_clear_error();
            const int n = SSL_write(mSsl, buf, static_cast<int>(len));
            if (n > 0) {
                buf += n;
                len -= static_cast<size_t>(n);
                continue;
            }
            const int err = SSL_get_error(mSsl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            if (err == SSL_ERROR_SYSCALL &&
                (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            mError = tlsError("tls write");
            return false;
        }
        return true;
    }

private:
    SSL* mSsl;
};

#endif // HTTP_SERVICE_HAVE_TLS

// -------------------------------------------------------------------------
// TCP connect
//
// Non-blocking connect + poll, because a blocking connect() to a black-holed
// address can sit there for minutes -- longer than any caller is prepared to
// wait, and not interruptible. Once connected the socket goes back to blocking
// with SO_RCVTIMEO, which is what gives recv() its kAgain slices.
// -------------------------------------------------------------------------
int connectTcp(const Url& url, int timeoutMs, std::string* error) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const int rc = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &results);
    if (rc != 0) {
        *error = "cannot resolve " + url.host + ": " + gai_strerror(rc);
        return -1;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(results, freeaddrinfo);

    const auto started = Clock::now();
    *error = "connect to " + url.host + ":" + url.port + " failed";

    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;

        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        bool connected = ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            const int64_t left = timeoutMs - elapsedMs(started);
            pollfd pfd = {fd, POLLOUT, 0};
            if (left > 0 && poll(&pfd, 1, static_cast<int>(left)) == 1) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) {
                    connected = true;
                } else {
                    errno = soerr;
                }
            } else if (left <= 0 || errno == 0) {
                errno = ETIMEDOUT;
            }
        }

        if (!connected) {
            *error = "connect to " + url.host + ":" + url.port + ": " + strerror(errno);
            close(fd);
            continue;
        }

        fcntl(fd, F_SETFL, flags);
        timeval tv = {kPollSliceMs / 1000, (kPollSliceMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        timeval sendTv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTv, sizeof(sendTv));
        error->clear();
        return fd;
    }
    return -1;
}

std::unique_ptr<Conn> openConnection(const Url& url, int timeoutMs, int* error,
                                     std::string* message) {
    const int fd = connectTcp(url, timeoutMs, message);
    if (fd < 0) {
        *error = kErrorConnect;
        return nullptr;
    }

    if (url.scheme != "https") return std::make_unique<PlainConn>(fd);

#ifdef HTTP_SERVICE_HAVE_TLS
    SSL_CTX* ctx = tlsContext();
    if (ctx == nullptr) {
        close(fd);
        *error = kErrorConnect;
        *message = "no usable TLS trust store on this host";
        return nullptr;
    }
    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        close(fd);
        *error = kErrorConnect;
        *message = tlsError("SSL_new");
        return nullptr;
    }
    // SNI, plus hostname checking as part of chain verification -- a valid
    // certificate for someone else is not a valid certificate for us.
    SSL_set_tlsext_host_name(ssl, url.host.c_str());
    SSL_set1_host(ssl, url.host.c_str());
    SSL_set_fd(ssl, fd);

    const auto started = Clock::now();
    while (true) {
        ERR_clear_error();
        const int rc = SSL_connect(ssl);
        if (rc == 1) break;
        const int err = SSL_get_error(ssl, rc);
        const bool retry = err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
                           (err == SSL_ERROR_SYSCALL &&
                            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK));
        if (retry && elapsedMs(started) < timeoutMs) continue;
        *message = retry ? "TLS handshake with " + url.host + " timed out"
                         : tlsError(("TLS handshake with " + url.host + " failed").c_str());
        SSL_free(ssl);
        close(fd);
        *error = kErrorConnect;
        return nullptr;
    }
    return std::make_unique<TlsConn>(fd, ssl);
#else
    close(fd);
    *error = kErrorBadUrl;
    *message = "https is not supported by this build (rebuild with OpenSSL available)";
    return nullptr;
#endif
}

// -------------------------------------------------------------------------
// Buffered reading
//
// Everything above works off one buffer: header lines are pulled out of it, and
// whatever is left when the headers end is the first slice of the body -- it
// arrived in the same packet and must not be dropped.
// -------------------------------------------------------------------------
class Reader {
public:
    Reader(Conn* conn, const CancelFn& isCanceled, int idleTimeoutMs)
        : mConn(conn), mIsCanceled(isCanceled), mIdleTimeoutMs(idleTimeoutMs) {
        mBuffer.resize(kReadBufferSize);
    }

    // kOk with *eof set, or an error code.
    int fill(bool* eof) {
        *eof = false;
        const auto idleSince = Clock::now();
        while (true) {
            if (mIsCanceled && mIsCanceled()) return kErrorCanceled;

            const ssize_t n = mConn->recv(mBuffer.data(), mBuffer.size());
            if (n > 0) {
                mAvailable = static_cast<size_t>(n);
                mOffset = 0;
                return kOk;
            }
            if (n == 0) {
                *eof = true;
                return kOk;
            }
            if (n == kAgain) {
                if (elapsedMs(idleSince) >= mIdleTimeoutMs) {
                    mError = "timed out waiting for data";
                    return kErrorIo;
                }
                continue;
            }
            mError = mConn->error();
            return kErrorIo;
        }
    }

    // One CRLF- (or LF-) terminated line, without the terminator.
    int readLine(std::string* line) {
        line->clear();
        while (true) {
            while (mOffset < mAvailable) {
                const char c = mBuffer[mOffset++];
                if (c == '\n') {
                    if (!line->empty() && line->back() == '\r') line->pop_back();
                    return kOk;
                }
                if (line->size() >= kMaxLineLength) {
                    mError = "response line too long";
                    return kErrorIo;
                }
                line->push_back(c);
            }
            bool eof = false;
            const int rc = fill(&eof);
            if (rc != kOk) return rc;
            if (eof) {
                mError = "connection closed mid-header";
                return kErrorIo;
            }
        }
    }

    // Bytes sitting in the buffer, already read from the socket.
    const char* buffered() const { return mBuffer.data() + mOffset; }
    size_t bufferedSize() const { return mAvailable - mOffset; }
    void consume(size_t n) { mOffset += n; }

    const std::string& error() const { return mError; }

private:
    Conn* mConn;
    const CancelFn& mIsCanceled;
    int mIdleTimeoutMs;
    std::vector<char> mBuffer;
    size_t mAvailable = 0;
    size_t mOffset = 0;
    std::string mError;
};

// -------------------------------------------------------------------------
// Response body
// -------------------------------------------------------------------------

// Hands `want` bytes (or everything until EOF when want < 0) to the sink.
int pumpPlain(Reader& reader, int64_t want, const BodyFn& onBody, int64_t* written,
              std::string* error) {
    while (want < 0 || *written < want) {
        if (reader.bufferedSize() == 0) {
            bool eof = false;
            const int rc = reader.fill(&eof);
            if (rc != kOk) {
                *error = reader.error();
                return rc;
            }
            if (eof) {
                if (want < 0) return kOk; // EOF *is* the framing here
                *error = "connection closed with " + std::to_string(want - *written) +
                         " bytes still expected";
                return kErrorIo;
            }
        }
        size_t chunk = reader.bufferedSize();
        if (want >= 0) chunk = std::min<size_t>(chunk, static_cast<size_t>(want - *written));
        if (!onBody(reader.buffered(), chunk)) {
            *error = "sink rejected the response body";
            return kErrorWrite;
        }
        reader.consume(chunk);
        *written += static_cast<int64_t>(chunk);
    }
    return kOk;
}

// Transfer-Encoding: chunked. Sizes are hex, may carry ";extensions", and each
// chunk is followed by its own CRLF.
int pumpChunked(Reader& reader, const BodyFn& onBody, int64_t* written, std::string* error) {
    while (true) {
        std::string header;
        int rc = reader.readLine(&header);
        if (rc != kOk) {
            *error = reader.error();
            return rc;
        }
        const size_t semi = header.find(';');
        if (semi != std::string::npos) header.resize(semi);
        header = trim(header);

        char* end = nullptr;
        const long long size = strtoll(header.c_str(), &end, 16);
        if (end == header.c_str() || size < 0) {
            *error = "malformed chunk size \"" + header + "\"";
            return kErrorIo;
        }
        if (size == 0) break;

        int64_t done = 0;
        rc = pumpPlain(reader, size, onBody, &done, error);
        *written += done;
        if (rc != kOk) return rc;

        std::string crlf;
        rc = reader.readLine(&crlf);
        if (rc != kOk) {
            *error = reader.error();
            return rc;
        }
    }
    // Trailers, then the terminating blank line.
    while (true) {
        std::string line;
        const int rc = reader.readLine(&line);
        if (rc != kOk) return kOk; // trailers are optional; the body is complete
        if (line.empty()) return kOk;
    }
}

// -------------------------------------------------------------------------
// One request/response exchange, no redirect following.
// -------------------------------------------------------------------------
struct Attempt {
    Result result;
    std::string redirectTo; // non-empty: caller should follow it
};

bool parseStatusLine(const std::string& line, int* code, std::string* reason) {
    if (line.compare(0, 5, "HTTP/") != 0) return false;
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) return false;
    *code = atoi(line.c_str() + sp + 1);
    const size_t sp2 = line.find(' ', sp + 1);
    *reason = sp2 == std::string::npos ? "" : trim(line.substr(sp2 + 1));
    return *code >= 100 && *code < 600;
}

// "bytes 200-1023/1024" -> start 200, total 1024. total is -1 for "*".
bool parseContentRange(const std::string& value, int64_t* start, int64_t* total) {
    const size_t sp = value.find(' ');
    if (sp == std::string::npos) return false;
    const std::string range = trim(value.substr(sp + 1));
    const size_t dash = range.find('-');
    const size_t slash = range.find('/');
    if (dash == std::string::npos || slash == std::string::npos || dash > slash) return false;
    *start = strtoll(range.c_str(), nullptr, 10);
    const std::string totalText = range.substr(slash + 1);
    *total = totalText == "*" ? -1 : strtoll(totalText.c_str(), nullptr, 10);
    return true;
}

Attempt performOnce(const Url& url, const Request& request, const HeadersFn& onHeaders,
                    const BodyFn& onBody, const CancelFn& isCanceled) {
    Attempt attempt;
    Result& result = attempt.result;

    int connectError = kOk;
    std::string message;
    std::unique_ptr<Conn> conn = openConnection(url, request.connectTimeoutMs, &connectError,
                                                &message);
    if (conn == nullptr) {
        result.error = connectError;
        result.message = message;
        return attempt;
    }

    std::string head = "GET " + url.target + " HTTP/1.1\r\n";
    head += "Host: " + url.hostHeader() + "\r\n";
    head += "User-Agent: " + std::string(kUserAgent) + "\r\n";
    head += "Accept: */*\r\n";
    // No gzip: this client hands bytes straight to a file, and a decompressed
    // body would not match the Content-Length the caller sizes its progress on.
    head += "Accept-Encoding: identity\r\n";
    if (request.resumeFrom > 0) {
        head += "Range: bytes=" + std::to_string(request.resumeFrom) + "-\r\n";
    }
    // No connection reuse, so no pool to manage and no half-read body to drain.
    head += "Connection: close\r\n\r\n";

    if (!conn->sendAll(head.data(), head.size())) {
        result.error = kErrorIo;
        result.message = "sending request failed: " + conn->error();
        return attempt;
    }

    Reader reader(conn.get(), isCanceled, request.idleTimeoutMs);

    std::string line;
    int rc = reader.readLine(&line);
    if (rc != kOk) {
        result.error = rc;
        result.message = reader.error();
        return attempt;
    }
    ResponseInfo& info = result.response;
    if (!parseStatusLine(line, &info.statusCode, &info.reason)) {
        result.error = kErrorIo;
        result.message = "not an HTTP response: \"" + line + "\"";
        return attempt;
    }

    Headers headers;
    while (true) {
        rc = reader.readLine(&line);
        if (rc != kOk) {
            result.error = rc;
            result.message = reader.error();
            return attempt;
        }
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue; // ignore junk rather than fail
        if (headers.size() >= kMaxHeaderCount) {
            result.error = kErrorTooLarge;
            result.message = "too many response headers";
            return attempt;
        }
        headers.add(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }

    // 3xx: hand the target back and let the caller decide whether to follow.
    // The body of a redirect is of no interest, and `Connection: close` means
    // closing the socket is all the cleanup there is.
    const bool redirect = info.statusCode == 301 || info.statusCode == 302 ||
                          info.statusCode == 303 || info.statusCode == 307 ||
                          info.statusCode == 308;
    if (redirect) {
        const std::string location = headers.get("Location");
        if (!location.empty()) {
            attempt.redirectTo = resolveRedirect(url, location);
            if (!attempt.redirectTo.empty()) return attempt;
        }
        result.error = kErrorIo;
        result.message = "HTTP " + std::to_string(info.statusCode) + " without a usable Location";
        return attempt;
    }

    if (info.statusCode >= 400) {
        result.error = kErrorHttpStatus;
        result.message = "HTTP " + std::to_string(info.statusCode) +
                         (info.reason.empty() ? "" : " " + info.reason);
        return attempt;
    }

    info.contentType = headers.get("Content-Type");
    info.finalUrl = url.toString();

    const std::string lengthText = headers.get("Content-Length");
    const int64_t contentLength = lengthText.empty() ? -1 : strtoll(lengthText.c_str(), nullptr, 10);

    if (info.statusCode == 206) {
        int64_t start = 0, total = -1;
        if (parseContentRange(headers.get("Content-Range"), &start, &total)) {
            info.startOffset = start;
            info.totalBytes = total;
        } else {
            // 206 without a parsable Content-Range: trust the Range we asked for.
            info.startOffset = request.resumeFrom;
            info.totalBytes = contentLength < 0 ? -1 : request.resumeFrom + contentLength;
        }
    } else {
        // 200 to a ranged request means the server ignored it and is sending the
        // whole resource; the body starts at 0 and anything already on disk is
        // about to be overwritten.
        info.startOffset = 0;
        info.totalBytes = contentLength;
    }

    if (onHeaders && !onHeaders(info)) {
        result.error = kErrorWrite;
        result.message = "caller refused the response";
        return attempt;
    }

    const bool chunked = headers.contains("Transfer-Encoding", "chunked");
    std::string bodyError;
    if (chunked) {
        rc = pumpChunked(reader, onBody, &result.bytes, &bodyError);
    } else {
        // No Content-Length and not chunked: the body runs until the connection
        // closes, which `Connection: close` guarantees will happen.
        rc = pumpPlain(reader, contentLength, onBody, &result.bytes, &bodyError);
    }
    if (rc != kOk) {
        result.error = rc;
        result.message = bodyError;
    }
    return attempt;
}

} // namespace

// -------------------------------------------------------------------------
// URLs
// -------------------------------------------------------------------------

std::string Url::hostHeader() const {
    const bool isDefault = (scheme == "http" && port == "80") ||
                           (scheme == "https" && port == "443");
    const bool literalV6 = host.find(':') != std::string::npos;
    const std::string bare = literalV6 ? "[" + host + "]" : host;
    return isDefault ? bare : bare + ":" + port;
}

std::string Url::toString() const {
    return scheme + "://" + hostHeader() + target;
}

bool parseUrl(const std::string& text, Url* out) {
    const std::string input = trim(text);
    const size_t schemeEnd = input.find("://");
    if (schemeEnd == std::string::npos) return false;

    Url url;
    url.scheme = toLower(input.substr(0, schemeEnd));
    if (url.scheme != "http" && url.scheme != "https") return false;

    const size_t authorityStart = schemeEnd + 3;
    size_t authorityEnd = input.find_first_of("/?#", authorityStart);
    if (authorityEnd == std::string::npos) authorityEnd = input.size();

    std::string authority = input.substr(authorityStart, authorityEnd - authorityStart);
    // Strip any userinfo; credentials in URLs are not supported, but a URL that
    // carries them must not be mistaken for a host named "user:pass@host".
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (authority.empty()) return false;

    if (authority.front() == '[') { // IPv6 literal
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        url.host = authority.substr(1, close - 1);
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty() && rest.front() == ':') url.port = rest.substr(1);
    } else {
        const size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            url.host = authority;
        } else {
            url.host = authority.substr(0, colon);
            url.port = authority.substr(colon + 1);
        }
    }
    if (url.host.empty()) return false;
    if (url.port.empty()) url.port = url.scheme == "https" ? "443" : "80";
    if (url.port.find_first_not_of("0123456789") != std::string::npos) return false;

    url.target = authorityEnd < input.size() ? input.substr(authorityEnd) : "/";
    if (url.target.empty() || url.target.front() != '/') url.target = "/" + url.target;
    const size_t fragment = url.target.find('#');
    if (fragment != std::string::npos) url.target.resize(fragment);

    *out = url;
    return true;
}

std::string resolveRedirect(const Url& base, const std::string& location) {
    const std::string target = trim(location);
    if (target.empty()) return {};
    if (target.find("://") != std::string::npos) return target; // absolute

    Url next = base;
    if (target.front() == '/') {
        next.target = target;
    } else {
        const size_t lastSlash = base.target.rfind('/');
        const std::string dir = lastSlash == std::string::npos
                                        ? "/"
                                        : base.target.substr(0, lastSlash + 1);
        next.target = dir + target;
    }
    return next.toString();
}

bool tlsSupported() {
#ifdef HTTP_SERVICE_HAVE_TLS
    return true;
#else
    return false;
#endif
}

// -------------------------------------------------------------------------
// GET, with redirects
// -------------------------------------------------------------------------

Result get(const Request& request, const HeadersFn& onHeaders, const BodyFn& onBody,
           const CancelFn& isCanceled) {
    Result result;
    std::string current = request.url;

    for (int hop = 0; hop <= request.maxRedirects; ++hop) {
        Url url;
        if (!parseUrl(current, &url)) {
            result.error = kErrorBadUrl;
            result.message = "cannot parse URL \"" + current + "\"";
            return result;
        }
        if (isCanceled && isCanceled()) {
            result.error = kErrorCanceled;
            result.message = "canceled";
            return result;
        }

        Request hopRequest = request;
        hopRequest.url = current;
        Attempt attempt = performOnce(url, hopRequest, onHeaders, onBody, isCanceled);
        if (attempt.redirectTo.empty()) return attempt.result;
        current = attempt.redirectTo;
    }

    result.error = kErrorTooLarge;
    result.message = "more than " + std::to_string(request.maxRedirects) + " redirects";
    return result;
}

Result getString(const std::string& url, size_t maxBytes, std::string* body) {
    body->clear();
    Request request;
    request.url = url;

    bool overflowed = false;
    Result result = get(
            request, nullptr,
            [&](const char* data, size_t len) {
                if (body->size() + len > maxBytes) {
                    overflowed = true;
                    return false;
                }
                body->append(data, len);
                return true;
            },
            nullptr);

    if (overflowed) {
        result.error = kErrorTooLarge;
        result.message = "response exceeds " + std::to_string(maxBytes) + " bytes";
        body->clear();
    }
    return result;
}

} // namespace httpsvc
