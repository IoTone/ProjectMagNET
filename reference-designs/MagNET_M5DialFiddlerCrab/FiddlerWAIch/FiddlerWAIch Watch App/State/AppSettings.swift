import Foundation
import SwiftUI

enum BrokerPreset: String, CaseIterable, Identifiable {
    case localhost
    case custom
    case hivemqPublic

    var id: String { rawValue }

    var url: String {
        switch self {
        case .localhost:    return "ws://localhost:9001/mqtt"
        case .custom:       return "" // provided by user
        case .hivemqPublic: return "ws://broker.hivemq.com:8000/mqtt"
        }
    }

    var displayKey: LocalizedStringKey {
        switch self {
        case .localhost:    return "broker.localhost"
        case .custom:       return "broker.custom"
        case .hivemqPublic: return "broker.hivemq_public"
        }
    }
}

enum FeedbackMode: String, CaseIterable, Identifiable {
    case haptics
    case audio
    case both
    case off

    var id: String { rawValue }

    var displayKey: LocalizedStringKey {
        switch self {
        case .haptics: return "feedback.haptics"
        case .audio:   return "feedback.audio"
        case .both:    return "feedback.both"
        case .off:     return "feedback.off"
        }
    }
}

@MainActor
final class AppSettings: ObservableObject {
    @AppStorage("mac4") var mac4: String = ""
    @AppStorage("brokerPreset") var brokerPresetRaw: String = BrokerPreset.localhost.rawValue
    @AppStorage("customBrokerURL") var customBrokerURL: String = ""
    @AppStorage("publicBrokerAcknowledged") var publicBrokerAcknowledged: Bool = false
    @AppStorage("clientId") var clientId: String = ""
    @AppStorage("feedbackMode") var feedbackModeRaw: String = FeedbackMode.haptics.rawValue

    var brokerPreset: BrokerPreset {
        get { BrokerPreset(rawValue: brokerPresetRaw) ?? .localhost }
        set { brokerPresetRaw = newValue.rawValue }
    }

    var feedbackMode: FeedbackMode {
        get { FeedbackMode(rawValue: feedbackModeRaw) ?? .haptics }
        set { feedbackModeRaw = newValue.rawValue }
    }

    var activeBrokerURL: String {
        switch brokerPreset {
        case .localhost:    return BrokerPreset.localhost.url
        case .custom:       return customBrokerURL
        case .hivemqPublic: return BrokerPreset.hivemqPublic.url
        }
    }

    func ensureClientId() {
        if clientId.isEmpty {
            clientId = "fiddlerwaich-" + UUID().uuidString.prefix(8).lowercased()
        }
    }

    static func isValidMac4(_ s: String) -> Bool {
        let pattern = #"^[0-9A-Za-z]{4}$"#
        return s.range(of: pattern, options: .regularExpression) != nil
    }

    /// Case-preserving alphanumeric filter for the 4-char channel label.
    /// MQTT topics are case-sensitive, so whatever the user types is what we publish/subscribe to.
    static func sanitizeMac4(_ s: String) -> String {
        String(s.filter { $0.isLetter || $0.isNumber }.prefix(4))
    }

    /// Timestamp of the executable, formatted MM-dd HH:mm.
    /// Changes every rebuild, so it doubles as a "which version am I running?" indicator.
    static var buildTimestamp: String {
        guard let exe = Bundle.main.executableURL,
              let attrs = try? FileManager.default.attributesOfItem(atPath: exe.path),
              let date = attrs[.modificationDate] as? Date else { return "?" }
        let fmt = DateFormatter()
        fmt.dateFormat = "MM-dd HH:mm"
        return fmt.string(from: date)
    }
}
