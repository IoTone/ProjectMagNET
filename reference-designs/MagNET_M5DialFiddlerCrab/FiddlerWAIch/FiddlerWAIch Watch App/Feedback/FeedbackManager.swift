import Foundation

@MainActor
final class FeedbackManager: ObservableObject {
    private let haptics = HapticManager()
    private let audio = AudioManager()
    private let settings: AppSettings

    private var lastEvent: [String: (state: SessionState, at: Date)] = [:]
    private let debounceInterval: TimeInterval = 2.0

    init(settings: AppSettings) {
        self.settings = settings
    }

    func stateTransition(to state: SessionState, visible: Bool, sessionId: String) {
        if let last = lastEvent[sessionId],
           last.state == state,
           Date().timeIntervalSince(last.at) < debounceInterval {
            return
        }
        lastEvent[sessionId] = (state, Date())
        dispatchTransition(to: state, visible: visible)
    }

    func swipeClick() {
        switch settings.feedbackMode {
        case .haptics: haptics.swipeClick()
        case .audio:   audio.swipeClick()
        case .both:    haptics.swipeClick(); audio.swipeClick()
        case .off:     break
        }
    }

    func connected() {
        switch settings.feedbackMode {
        case .haptics: haptics.connected()
        case .audio:   audio.connected()
        case .both:    haptics.connected(); audio.connected()
        case .off:     break
        }
    }

    func disconnected() {
        switch settings.feedbackMode {
        case .haptics: haptics.disconnected()
        case .audio:   audio.disconnected()
        case .both:    haptics.disconnected(); audio.disconnected()
        case .off:     break
        }
    }

    private func dispatchTransition(to state: SessionState, visible: Bool) {
        switch settings.feedbackMode {
        case .haptics: haptics.stateTransition(to: state, visible: visible)
        case .audio:   audio.stateTransition(to: state)
        case .both:
            haptics.stateTransition(to: state, visible: visible)
            audio.stateTransition(to: state)
        case .off:
            break
        }
    }
}
