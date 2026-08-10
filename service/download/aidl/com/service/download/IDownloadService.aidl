package com.service.download;

import com.service.download.IDownloadCallback;

/**
 * Download manager, registered with servicemanager as "download.service".
 *
 * It does no networking of its own: it looks up "http.service" and drives it,
 * so a download is a binder call that makes another binder call. What this
 * service adds on top is the file side of the job -- a .part file, resume after
 * an interrupted attempt, the atomic rename at the end -- plus queueing and
 * progress aggregation.
 *
 * enqueue() returns as soon as the job is accepted; everything after that
 * arrives on IDownloadCallback.
 */
interface IDownloadService {
    /** Name to register/look up with servicemanager; here so the two sides cannot drift. */
    const String SERVICE_NAME = "download.service";

    /** Accepted, not started. */
    const int STATE_PENDING = 0;
    /** Transferring. */
    const int STATE_RUNNING = 1;
    /** Done; the file is at its final path. */
    const int STATE_COMPLETED = 2;
    /** Failed; see IDownloadCallback.onFailed for why. */
    const int STATE_FAILED = 3;
    /** Canceled; the .part file survives for a resume. */
    const int STATE_CANCELED = 4;
    /** No download has ever had this id. */
    const int STATE_UNKNOWN = -1;

    const int OK = 0;
    /** destPath is empty, or its directory cannot be created/written. */
    const int ERROR_BAD_DESTINATION = -1;
    /** open()/write() on the .part file failed, or the final rename did. */
    const int ERROR_IO = -2;
    /** "http.service" is not registered, or the call to it failed. */
    const int ERROR_HTTP_SERVICE = -3;
    /** The transport reported a failure; the message carries its error text. */
    const int ERROR_HTTP = -4;
    /** cancel() was called. */
    const int ERROR_CANCELED = -5;

    /**
     * Queues a download and returns immediately with its id.
     *
     * A `<destPath>.part` file is written during the transfer and renamed to
     * `destPath` on success; if one is already there from an earlier attempt,
     * the download resumes from its length.
     *
     * @param callback  progress sink, or null to run silently (poll getState()).
     * @return          the download id, used with every other method here.
     */
    int enqueue(@utf8InCpp String url, @utf8InCpp String destPath,
                @nullable IDownloadCallback callback);

    /**
     * Stops a running download; its callback gets onCanceled(). No-op once the
     * download has reached a terminal state.
     */
    oneway void cancel(int downloadId);

    /** One of STATE_*. */
    int getState(int downloadId);

    /** Bytes on disk right now, including a resumed prefix. -1 if unknown id. */
    long getDownloadedBytes(int downloadId);

    /**
     * Blocks until the download reaches a terminal state, or `timeoutMs`
     * elapses (<= 0 waits forever). Returns the state at that point.
     *
     * Only for callers that want a synchronous download; with a callback there
     * is nothing to wait for.
     */
    int awaitCompletion(int downloadId, long timeoutMs);

    /** One-line human-readable status, for logs and debugging. */
    @utf8InCpp String describe(int downloadId);

    /** Ids known to the service, newest last. */
    int[] getDownloadIds();
}
