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
}
