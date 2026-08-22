package com.miui.home.library.utils;

/** Fake of the CN launcher's AsyncTaskExecutorHelper (test shape only). */
public final class AsyncTaskExecutorHelper {
    public static final EventBus bus = new EventBus();

    public static EventBus getEventBus() { return bus; }

    /** Minimal EventBus shape: records posted messages. */
    public static final class EventBus {
        public int posts;

        public void post(Object message) { posts++; }
    }
}
