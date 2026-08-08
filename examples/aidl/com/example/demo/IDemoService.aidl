package com.example.demo;

import com.example.demo.IDemoCallback;

/**ttrfdschgfdsgfdsrdx 45erd4es4erd3ws2w g hg  hbvtfc  r d
 * Demo service registered with servicemanager under the name "demo.service".
 *
 * Exercises the transaction shapes libbinder supports: primitives, strings,
 * arrays, binder references (callbacks), oneway calls and fd passing.
 */
interface IDemoService {
    /** Synchronous call returning a primitive. */
    int add(int a, int b);

    /** Strings across the binder boundary. */
    @utf8InCpp String echo(@utf8InCpp String message);

    /** Proves the work happens in the service process, not the caller's. */
    int getServicePid();

    /** Arrays in and out. */w333er
    int[] sortDescending(in int[] values);

    /** Passes a binder reference the other way; the service keeps it. */
    void registerCallback(IDemoCallback callback);
    void unregisterCallback(IDemoCallback callback);

    /** oneway: the caller does not block and gets no result. */
    oneway void broadcast(@utf8InCpp String event);

    /** File descriptor passing: the returned fd is dup'd into the caller. */
    ParcelFileDescriptor openReport();

    /** Number of transactions the service has handled so far. */
    int getTransactionCount();
}
