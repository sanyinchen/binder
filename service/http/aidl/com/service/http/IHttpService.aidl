package com.service.http;

import com.service.http.IHttpProgressCallback;

/**
 * HTTP transport, registered with servicemanager as "http.service".
 *
 * It owns the sockets and the HTTP/1.1 parsing; callers only ever see a status
 * code and bytes. The download service is its main client -- nothing here knows
 * what a download is.
 *
 * Bodies never travel inside a Parcel: fetchToFd() writes into a file
 * descriptor the caller passes in, so a 500 MB response costs the same binder
 * traffic as a 500 byte one. (A binder transaction buffer is ~1 MB per process,
 * shared by every in-flight call.) getString() is the exception, for small
 * bodies, and is capped.
 */
interface IHttpService {
    /** Name to register/look up with servicemanager; here so the two sides cannot drift. */
    const String SERVICE_NAME = "http.service";

    /** Nothing went wrong. */
    const int OK = 0;
    /** Malformed or unsupported URL (only http:// and https:// are handled). */
    const int ERROR_BAD_URL = -1;
    /** DNS lookup, connect() or TLS handshake failed. */
    const int ERROR_CONNECT = -2;
    /** Connection died mid-transfer, or the response was not valid HTTP. */
    const int ERROR_IO = -3;
    /** Server answered, but with a status >= 400. */
    const int ERROR_HTTP_STATUS = -4;
    /** cancel() was called for this request id. */
    const int ERROR_CANCELED = -5;
    /** Writing to the caller's fd failed (disk full, bad fd). */
    const int ERROR_WRITE = -6;
    /** Too many redirects, or the response exceeded getString()'s cap. */
    const int ERROR_TOO_LARGE = -7;

    /**
     * Reserves an id for a request that has not started yet.
     *
     * cancel() needs a handle on a request, and fetchToFd() blocks until the
     * transfer is done -- so the id has to exist before the call that uses it.
     */
    int newRequestId();

    /**
     * GET `url` and return the body as a string. For small responses (config,
     * API replies); fails with ERROR_TOO_LARGE past 1 MiB. Use fetchToFd() for
     * anything else.
     */
    @utf8InCpp String getString(@utf8InCpp String url);

    /**
     * GET `url`, writing the body into `sink`. Blocks until the transfer
     * finishes, fails, or is canceled; the caller should not be on a thread it
     * needs for anything else.
     *
     * Redirects (301/302/303/307/308) are followed. `sink` is positioned by the
     * service (see IHttpProgressCallback.onResponseStart) -- the caller's file
     * offset is not used.
     *
     * @param requestId   from newRequestId(); addresses this transfer in cancel().
     * @param resumeFrom  byte offset to resume from, sent as `Range: bytes=N-`.
     *                    0 for a fresh download. The server may ignore it.
     * @param callback    progress sink, or null for none.
     * @return            body bytes written by this call. On failure the status
     *                    carries one of the ERROR_* codes as its service-specific
     *                    error, and the message says what happened.
     */
    long fetchToFd(int requestId, @utf8InCpp String url, in ParcelFileDescriptor sink,
                   long resumeFrom, @nullable IHttpProgressCallback callback);

    /**
     * Aborts the transfer running under `requestId`; its fetchToFd() fails with
     * ERROR_CANCELED. A no-op for ids that are unknown or already finished.
     *
     * oneway because the caller is by definition on another thread than the
     * blocked fetchToFd() -- making it synchronous would just make the canceller
     * wait for the socket read to unblock.
     */
    oneway void cancel(int requestId);

    /** Requests currently executing. Handy as a liveness check. */
    int getActiveRequestCount();
}
