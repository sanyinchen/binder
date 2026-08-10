package com.service.http;

/**
 * Progress reporting for a request served by IHttpService.
 *
 * Implemented by the *caller* of fetchToFd() -- the download service -- and
 * passed across binder, so the http service calls back into that process.
 *
 * oneway: the http service must never block on a progress report; it is in the
 * middle of draining a socket. A slow (or dead) listener costs it nothing.
 */
oneway interface IHttpProgressCallback {
    /**
     * Response headers are in. Called once per request, before any
     * onBytesReceived().
     *
     * @param startOffset  where the body lands in the file: `resumeFrom` when
     *                     the server honoured the Range request (206), 0 when
     *                     it ignored it and sent the whole thing (200).
     * @param totalBytes   size of the complete resource, or -1 if the server
     *                     did not say (no Content-Length, chunked encoding).
     */
    void onResponseStart(int requestId, int statusCode, long startOffset, long totalBytes,
                         @utf8InCpp String contentType);

    /**
     * Body bytes written so far. Rate-limited by the service; do not expect one
     * call per read().
     *
     * @param received  bytes of *this* response written, excluding startOffset.
     * @param total     same meaning as onResponseStart's totalBytes.
     */
    void onBytesReceived(int requestId, long received, long total);
}
