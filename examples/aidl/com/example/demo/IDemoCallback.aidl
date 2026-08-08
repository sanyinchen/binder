package com.example.demo;

/**
 * Client-implemented callback. The client passes an instance of this across
 * binder to the service, which then calls back into the client process.
 */
interface IDemoCallback {
    /** Fire-and-forget: returns immediately without waiting for the client. */
    oneway void onEvent(@utf8InCpp String event);
}
