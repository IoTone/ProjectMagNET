import Foundation

/// Rolling in-app log buffer. Captures the last N [MQTT-WS] events so they can be rendered
/// on-device without needing Xcode attached.
@MainActor
final class MQTTLog: ObservableObject {
    static let shared = MQTTLog()
    @Published private(set) var entries: [String] = []
    private let capacity = 12

    func append(_ line: String) {
        print(line) // still log to Xcode console
        let fmt = DateFormatter()
        fmt.dateFormat = "HH:mm:ss"
        let stamped = "\(fmt.string(from: Date())) \(line)"
        entries.append(stamped)
        while entries.count > capacity {
            entries.removeFirst()
        }
    }

    var last5: [String] { Array(entries.suffix(5)) }

    /// `localizedDescription` alone is user-facing prose in the device language, which on a
    /// ja-only build hides the one detail that matters. -1009 (denied by network policy) and
    /// -1004 (nothing listening) are entirely different problems and read identically
    /// otherwise. Keep the domain and numeric code.
    nonisolated static func describe(_ error: Error) -> String {
        let ns = error as NSError
        var out = "\(ns.domain) \(ns.code)"
        if let underlying = ns.userInfo[NSUnderlyingErrorKey] as? NSError {
            out += " (under: \(underlying.domain) \(underlying.code))"
        }
        return out + " — " + ns.localizedDescription
    }
}
