import XCTest
@testable import FiddlerWAIch_Watch_App

final class MQTTPacketTests: XCTestCase {

    private func publishPacket(topic: String, payload: String) -> Data {
        let topicBytes = Array(topic.utf8)
        var body = Data()
        body.append(UInt8((topicBytes.count >> 8) & 0xFF))
        body.append(UInt8(topicBytes.count & 0xFF))
        body.append(contentsOf: topicBytes)
        body.append(contentsOf: Array(payload.utf8))

        var packet = Data()
        packet.append(0x30) // PUBLISH, qos 0
        packet.append(MQTTPacket.encodeRemainingLength(body.count))
        packet.append(body)
        return packet
    }

    func testDecodeConnackAccepted() {
        guard case .connack(let ok)? = MQTTPacket.decode(Data([0x20, 0x02, 0x00, 0x00])) else {
            return XCTFail("expected connack")
        }
        XCTAssertTrue(ok)
    }

    func testDecodePublish() {
        let pkt = publishPacket(topic: "iotj/cl/openwr/updates/b7a4/id1", payload: "2|opus|1|2|0|h")
        guard case .publish(let topic, let payload)? = MQTTPacket.decode(pkt) else {
            return XCTFail("expected publish")
        }
        XCTAssertEqual(topic, "iotj/cl/openwr/updates/b7a4/id1")
        XCTAssertEqual(String(data: payload, encoding: .utf8), "2|opus|1|2|0|h")
    }

    /// Regression: Data's Int subscript is absolute, so a packet handed to the decoder as a
    /// slice with a non-zero startIndex used to read the wrong bytes — or trap outright.
    func testDecodeToleratesNonZeroStartIndex() {
        var stream = Data([0xFF, 0xFF, 0xFF, 0xFF])
        let pkt = publishPacket(topic: "a/b", payload: "hi")
        stream.append(pkt)
        let slice = stream.dropFirst(4)
        XCTAssertNotEqual(slice.startIndex, 0)

        guard case .publish(let topic, let payload)? = MQTTPacket.decode(slice) else {
            return XCTFail("expected publish from offset slice")
        }
        XCTAssertEqual(topic, "a/b")
        XCTAssertEqual(String(data: payload, encoding: .utf8), "hi")
    }

    /// Two packets arriving in one transport read must both decode. This is the case that
    /// crashed: the buffer left over after the first packet started at a non-zero index.
    func testBackToBackPacketsInOneBuffer() {
        var buf = Data()
        buf.append(Data([0x20, 0x02, 0x00, 0x00]))                  // CONNACK
        buf.append(Data([0x90, 0x03, 0x00, 0x02, 0x01]))            // SUBACK
        buf.append(publishPacket(topic: "a/b", payload: "x"))       // PUBLISH

        var decoded: [String] = []
        while let (packet, consumed) = Self.frameNextPacket(from: buf) {
            buf = Data(buf.dropFirst(consumed))
            switch MQTTPacket.decode(packet) {
            case .connack:            decoded.append("connack")
            case .suback:             decoded.append("suback")
            case .publish(let t, _):  decoded.append("publish:\(t)")
            default:                  decoded.append("other")
            }
        }
        XCTAssertEqual(decoded, ["connack", "suback", "publish:a/b"])
        XCTAssertTrue(buf.isEmpty)
    }

    /// Mirror of the private framer in MQTTClient / MQTTClientTCP.
    private static func frameNextPacket(from buf: Data) -> (Data, Int)? {
        let base = buf.startIndex
        guard buf.count >= 2 else { return nil }
        var idx = 1
        var multiplier = 1
        var remaining = 0
        var loop = 0
        while idx < buf.count {
            let b = buf[base + idx]
            idx += 1
            remaining += Int(b & 0x7F) * multiplier
            if (b & 0x80) == 0 { break }
            multiplier *= 128
            loop += 1
            if loop > 3 { return nil }
            if idx >= buf.count { return nil }
        }
        let total = idx + remaining
        guard buf.count >= total else { return nil }
        return (Data(buf[base ..< (base + total)]), total)
    }

    func testEncodeRemainingLengthMultiByte() {
        XCTAssertEqual(Array(MQTTPacket.encodeRemainingLength(0)), [0x00])
        XCTAssertEqual(Array(MQTTPacket.encodeRemainingLength(127)), [0x7F])
        XCTAssertEqual(Array(MQTTPacket.encodeRemainingLength(128)), [0x80, 0x01])
        XCTAssertEqual(Array(MQTTPacket.encodeRemainingLength(16383)), [0xFF, 0x7F])
    }
}
