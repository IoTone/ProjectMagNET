import Foundation

enum MQTTConfig {
    static let keepAliveSeconds: UInt16 = 60
    static let qos: UInt8 = 1

    /// Everything before the `<mac4>/<sessionId>` tail.
    static let topicPrefix = "iotj/cl/openwr/updates"

    static func topicPattern(mac4: String) -> String {
        "\(topicPrefix)/\(mac4)/#"
    }

    static func baseTopic(mac4: String) -> String {
        "\(topicPrefix)/\(mac4)"
    }
}

enum ConnectionState: Equatable {
    case disconnected
    case connecting
    case connected
}
