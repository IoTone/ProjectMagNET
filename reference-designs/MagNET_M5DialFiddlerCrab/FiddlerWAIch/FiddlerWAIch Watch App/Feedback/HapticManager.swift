import WatchKit

struct HapticManager {
    func play(_ type: WKHapticType) {
        WKInterfaceDevice.current().play(type)
    }

    func stateTransition(to state: SessionState, visible: Bool) {
        switch state {
        case .working:
            play(.directionUp)
        case .finished:
            play(.success)
        case .needInput:
            // Double-tap so user feels it even if wrist is down for a moment.
            play(.notification)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [self] in play(.notification) }
        case .error:
            play(.failure)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [self] in play(.failure) }
        case .idle:
            play(.click)
        }
    }

    func swipeClick()     { play(.click) }
    func connected()      { play(.start) }
    func disconnected()   { play(.stop) }
}
