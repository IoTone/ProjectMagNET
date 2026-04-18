import Foundation

/// Minimal MQTT 3.1.1 packet encode/decode for a subscribe-only client.
/// Implements only: CONNECT, CONNACK, SUBSCRIBE, SUBACK, PUBLISH (recv), PINGREQ, PINGRESP, DISCONNECT.
enum MQTTPacket {

    enum PacketType: UInt8 {
        case connect     = 0x10
        case connack     = 0x20
        case publish     = 0x30
        case subscribe   = 0x82
        case suback      = 0x90
        case pingreq     = 0xC0
        case pingresp    = 0xD0
        case disconnect  = 0xE0
    }

    // MARK: - Encoders

    static func connect(clientId: String, keepAlive: UInt16) -> Data {
        var vh = Data()
        vh.appendMQTTString("MQTT")
        vh.append(0x04) // protocol level 4 (MQTT 3.1.1)
        vh.append(0x02) // flags: clean session
        vh.appendUInt16(keepAlive)

        var payload = Data()
        payload.appendMQTTString(clientId)

        var body = Data()
        body.append(vh)
        body.append(payload)

        var packet = Data()
        packet.append(PacketType.connect.rawValue)
        packet.append(encodeRemainingLength(body.count))
        packet.append(body)
        return packet
    }

    static func subscribe(topic: String, qos: UInt8, packetId: UInt16) -> Data {
        var body = Data()
        body.appendUInt16(packetId)
        body.appendMQTTString(topic)
        body.append(qos & 0x03)

        var packet = Data()
        packet.append(PacketType.subscribe.rawValue)
        packet.append(encodeRemainingLength(body.count))
        packet.append(body)
        return packet
    }

    static func pingreq() -> Data {
        return Data([PacketType.pingreq.rawValue, 0x00])
    }

    static func disconnect() -> Data {
        return Data([PacketType.disconnect.rawValue, 0x00])
    }

    // MARK: - Decoder

    enum Decoded {
        case connack(success: Bool)
        case suback(packetId: UInt16)
        case publish(topic: String, payload: Data)
        case pingresp
        case other
    }

    /// Decode a full MQTT control packet. Caller must have already framed the packet
    /// (i.e., `data` is exactly one complete packet).
    static func decode(_ data: Data) -> Decoded? {
        guard data.count >= 2 else { return nil }
        let firstByte = data[0]
        let ptype = firstByte & 0xF0

        // Skip fixed header: 1 byte + variable-length remaining length
        var idx = 1
        var multiplier = 1
        var remaining = 0
        var loop = 0
        while idx < data.count {
            let b = data[idx]
            idx += 1
            remaining += Int(b & 0x7F) * multiplier
            if (b & 0x80) == 0 { break }
            multiplier *= 128
            loop += 1
            if loop > 3 { return nil } // malformed
        }
        guard idx + remaining <= data.count else { return nil }
        let body = data.subdata(in: idx..<(idx + remaining))

        switch ptype {
        case 0x20: // CONNACK
            guard body.count >= 2 else { return nil }
            return .connack(success: body[1] == 0x00)
        case 0x90: // SUBACK
            guard body.count >= 3 else { return nil }
            let pid = (UInt16(body[0]) << 8) | UInt16(body[1])
            return .suback(packetId: pid)
        case 0x30: // PUBLISH
            return decodePublish(fixedByte: firstByte, body: body)
        case 0xD0: // PINGRESP
            return .pingresp
        default:
            return .other
        }
    }

    private static func decodePublish(fixedByte: UInt8, body: Data) -> Decoded? {
        let qos = (fixedByte >> 1) & 0x03
        guard body.count >= 2 else { return nil }
        let topicLen = Int(body[0]) << 8 | Int(body[1])
        guard body.count >= 2 + topicLen else { return nil }
        let topicBytes = body.subdata(in: 2..<(2 + topicLen))
        guard let topic = String(data: topicBytes, encoding: .utf8) else { return nil }
        var payloadStart = 2 + topicLen
        if qos > 0 {
            guard body.count >= payloadStart + 2 else { return nil }
            payloadStart += 2 // skip packet identifier
        }
        let payload = body.subdata(in: payloadStart..<body.count)
        return .publish(topic: topic, payload: payload)
    }

    // MARK: - Helpers

    static func encodeRemainingLength(_ length: Int) -> Data {
        var value = length
        var out = Data()
        repeat {
            var byte = UInt8(value % 128)
            value /= 128
            if value > 0 { byte |= 0x80 }
            out.append(byte)
        } while value > 0
        return out
    }
}

private extension Data {
    mutating func appendUInt16(_ v: UInt16) {
        append(UInt8((v >> 8) & 0xFF))
        append(UInt8(v & 0xFF))
    }
    mutating func appendMQTTString(_ s: String) {
        let bytes = Array(s.utf8)
        appendUInt16(UInt16(bytes.count))
        append(contentsOf: bytes)
    }
}
