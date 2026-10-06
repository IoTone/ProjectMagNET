import Foundation
import SwiftUI

enum BrokerPreset: String, CaseIterable, Identifiable {
    case localhost
    case custom
    case hivemqPublic
    case hivemqPublicTLS

    var id: String { rawValue }

    var url: String {
        switch self {
        case .localhost:    return "ws://localhost:9001/mqtt"
        case .custom:       return "" // provided by user
        case .hivemqPublic: return "ws://broker.hivemq.com:8000/mqtt"
        case .hivemqPublicTLS: return "wss://broker.hivemq.com:8884/mqtt"
        }
    }

    /// Both HiveMQ presets are world-readable and need the same confirmation.
    var isPublic: Bool { self == .hivemqPublic || self == .hivemqPublicTLS }

    var displayKey: LocalizedStringKey {
        switch self {
        case .localhost:    return "broker.localhost"
        case .custom:       return "broker.custom"
        case .hivemqPublic: return "broker.hivemq_public"
        case .hivemqPublicTLS: return "broker.hivemq_public_tls"
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
    // NOTE: these are deliberately NOT @AppStorage.
    //
    // @AppStorage is a DynamicProperty: it only wires itself into SwiftUI's update
    // machinery when it lives on a View. Declared on a class it still reads and writes
    // UserDefaults correctly, but it never fires objectWillChange — so every view that
    // observes AppSettings (ContentView's onboarding-vs-pager switch, the broker and
    // feedback radio buttons in SettingsView) kept rendering the old value forever.
    // Hand-rolled UserDefaults accessors below keep the same defaults keys — existing
    // installs keep their stored values — and publish properly.
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    private func string(_ key: String, _ fallback: String) -> String {
        defaults.string(forKey: key) ?? fallback
    }

    private func set<T>(_ value: T, _ key: String) {
        objectWillChange.send()
        defaults.set(value, forKey: key)
    }

    var mac4: String {
        get { string("mac4", "") }
        set { set(newValue, "mac4") }
    }

    // Default to the public broker: the hook script's own default CLAW_BROKER is
    // broker.hivemq.com, so this is the pairing that works with zero extra setup.
    // `localhost` as a default was never reachable from a watch — on-device it means the
    // watch itself, and it only ever resolved to the Mac in the simulator.
    var brokerPresetRaw: String {
        get { string("brokerPreset", BrokerPreset.hivemqPublicTLS.rawValue) }
        set { set(newValue, "brokerPreset") }
    }

    var customBrokerURL: String {
        get { string("customBrokerURL", "") }
        set { set(newValue, "customBrokerURL") }
    }

    var publicBrokerAcknowledged: Bool {
        get { defaults.bool(forKey: "publicBrokerAcknowledged") }
        set { set(newValue, "publicBrokerAcknowledged") }
    }

    var clientId: String {
        get { string("clientId", "") }
        set { set(newValue, "clientId") }
    }

    var feedbackModeRaw: String {
        get { string("feedbackMode", FeedbackMode.haptics.rawValue) }
        set { set(newValue, "feedbackMode") }
    }

    var brokerPreset: BrokerPreset {
        get { BrokerPreset(rawValue: brokerPresetRaw) ?? .hivemqPublicTLS }
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
        case .hivemqPublicTLS: return BrokerPreset.hivemqPublicTLS.url
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
