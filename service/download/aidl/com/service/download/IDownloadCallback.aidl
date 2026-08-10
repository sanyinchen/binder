package com.service.download;

/**
 * Download progress, implemented by the client and called back into the client
 * process by the download service.
 *
 * oneway throughout: the service reports from the thread running the transfer,
 * and a client that is slow to paint a progress bar must not slow the download
 * down. Ordering still holds -- oneway transactions to one binder are delivered
 * in the order they were made -- so onStarted always precedes onProgress.
 */
oneway interface IDownloadCallback {
    /**
     * The transfer is live and the server has answered.
     *
     * @param totalBytes  full size of the resource, or -1 when the server did
     *                    not say (then percent is -1 throughout).
     * @param resumedFrom bytes already on disk from an earlier attempt, which
     *                    this run picked up instead of re-fetching.
     */
    void onStarted(int downloadId, @utf8InCpp String url, long totalBytes, long resumedFrom);

    /**
     * Rate-limited progress (a few times a second at most, not per packet).
     *
     * @param downloadedBytes  bytes on disk, including any resumed prefix.
     * @param percent          0..100, or -1 when the total is unknown.
     * @param bytesPerSecond   throughput over the last interval.
     */
    void onProgress(int downloadId, long downloadedBytes, long totalBytes, int percent,
                    long bytesPerSecond);

    /** Terminal: the file is complete and renamed to its final path. */
    void onCompleted(int downloadId, @utf8InCpp String path, long totalBytes, long elapsedMs);

    /** Terminal: gave up. `errorCode` is one of IDownloadService.ERROR_*. */
    void onFailed(int downloadId, int errorCode, @utf8InCpp String message);

    /** Terminal: cancel() won. The partial file is kept for a later resume. */
    void onCanceled(int downloadId, long downloadedBytes);
}
