import Foundation

enum MQTTConfig {
    static let keepAliveSeconds: UInt16 = 60
    static let qos: UInt8 = 1

    static func topicPattern(mac4: String) -> String {
        "iotj/cl/openwr/updates/\(mac4)/#"
    }

    static func baseTopic(mac4: String) -> String {
        "iotj/cl/openwr/updates/\(mac4)"
    }
}

enum ConnectionState: Equatable {
    case disconnected
    case connecting
    case connected
}
