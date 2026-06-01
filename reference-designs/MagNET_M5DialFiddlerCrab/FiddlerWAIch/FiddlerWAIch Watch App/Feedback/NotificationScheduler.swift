import Foundation

/// Phase I no-op stub. Phase II will dispatch local notifications
/// (APNs relay or on-watch scheduling) for locked-screen alerts.
struct NotificationScheduler {
    func notifyStateTransition(session: Session, from prev: SessionState, to next: SessionState) {
        // Intentionally empty for Phase I.
    }
}
