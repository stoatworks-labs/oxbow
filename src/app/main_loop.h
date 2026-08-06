#pragma once

namespace oxbow {

/// Wait roughly `seconds`, servicing the main thread's run loop while doing so.
/// Call only from the main thread.
///
/// On macOS this is not a nicety. Syphon's discovery works by
/// NSDistributedNotificationCenter, and those notifications are delivered on
/// the **main** run loop — a worker thread running its own CFRunLoop does not
/// receive them, which was measured here: a server created on a private
/// run-loop thread is well-formed, announces itself, and is then invisible to
/// every consumer, while the identical code on the main thread is found at
/// once. The Syphon backend therefore creates its server on the main thread via
/// dispatch, and that dispatch only completes if the main thread is inside a
/// run loop rather than a plain sleep.
///
/// So: every place the main thread waits must wait *here*. A plain
/// `sleep_for` on the main thread will deadlock a `dispatch_sync` coming from
/// the frame thread.
///
/// Elsewhere this is exactly a sleep.
void waitServicingMainLoop(double seconds);

}  // namespace oxbow
