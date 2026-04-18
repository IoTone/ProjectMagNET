import SwiftUI

enum SessionState: Int, Sendable {
    case idle      = 0
    case working   = 2
    case needInput = 3
    case finished  = 5
    case error     = 7

    init(raw: Int) {
        self = SessionState(rawValue: raw) ?? .idle
    }

    var localizedKey: LocalizedStringKey {
        switch self {
        case .idle:      return "state.idle"
        case .working:   return "state.working"
        case .needInput: return "state.need_input"
        case .finished:  return "state.finished"
        case .error:     return "state.error"
        }
    }

    var accentColor: Color {
        switch self {
        case .idle:      return Theme.dimGray
        case .working:   return Theme.neonGreen
        case .needInput: return Theme.yellow
        case .finished:  return Theme.cyan
        case .error:     return Theme.red
        }
    }

    enum EyeStyle { case closed, lookingAround, squinting, wideToSmall, asymmetric }

    var eyeStyle: EyeStyle {
        switch self {
        case .idle:      return .closed
        case .working:   return .squinting
        case .needInput: return .lookingAround
        case .finished:  return .wideToSmall
        case .error:     return .asymmetric
        }
    }
}
