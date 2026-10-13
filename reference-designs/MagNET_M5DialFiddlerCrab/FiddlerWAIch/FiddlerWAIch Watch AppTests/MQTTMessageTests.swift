import XCTest
@testable import FiddlerWAIch_Watch_App

final class MQTTMessageTests: XCTestCase {
    func testParseLegacySixFields() {
        let topic = "iotj/cl/openwr/updates/b7a4/308c0f30-4ec0-4f9d-9145-08d5fe87ae1d"
        let payload = "2|opus-4-6|35|-1|0|dkords-laptop"
        let msg = MQTTMessage.parse(topic: topic, payload: payload)
        XCTAssertNotNil(msg)
        XCTAssertEqual(msg?.mac4, "b7a4")
        XCTAssertEqual(msg?.sessionId, "308c0f30-4ec0-4f9d-9145-08d5fe87ae1d")
        XCTAssertEqual(msg?.state, .working)
        XCTAssertEqual(msg?.model, "opus-4-6")
        XCTAssertEqual(msg?.sessionPct, 35)
        XCTAssertEqual(msg?.weeklyPct, -1)
        XCTAssertEqual(msg?.clientHost, "dkords-laptop")
        XCTAssertEqual(msg?.promptPreview, "")
    }

    func testParseExtendedSevenFields() {
        let topic = "iotj/cl/openwr/updates/b7a4/abc123"
        let payload = "3|sonnet-4-6|12|-1|0|laptop|Fix the bug in the MQTT reconnect"
        let msg = MQTTMessage.parse(topic: topic, payload: payload)
        XCTAssertEqual(msg?.state, .needInput)
        XCTAssertEqual(msg?.promptPreview, "Fix the bug in the MQTT reconnect")
    }

    func testParseMalformedFieldCount() {
        let topic = "iotj/cl/openwr/updates/b7a4/id1"
        XCTAssertNil(MQTTMessage.parse(topic: topic, payload: "bad"))
        XCTAssertNil(MQTTMessage.parse(topic: topic, payload: "1|2|3"))
    }

    func testParseMalformedState() {
        let topic = "iotj/cl/openwr/updates/b7a4/id1"
        XCTAssertNil(MQTTMessage.parse(topic: topic, payload: "abc|m|0|0|0|h"))
    }

    func testUnknownStateCodeMapsToIdle() {
        let topic = "iotj/cl/openwr/updates/b7a4/id1"
        let msg = MQTTMessage.parse(topic: topic, payload: "99|m|0|0|0|h")
        XCTAssertEqual(msg?.state, .idle)
    }

    func testEmptyTopicSegmentsRejected() {
        XCTAssertNil(MQTTMessage.parse(topic: "justone", payload: "0|m|0|0|0|h"))
    }

    /// `#` matches the parent level too, so the bare base topic shows up on the
    /// subscription. It has no session segment and must not become a session.
    func testParentTopicWithoutSessionSegmentRejected() {
        XCTAssertNil(MQTTMessage.parse(topic: "iotj/cl/openwr/updates/b7a4",
                                       payload: "2|unknown|-1|-1|0|MacBook-BR0"))
    }

    func testForeignTopicPrefixRejected() {
        XCTAssertNil(MQTTMessage.parse(topic: "somewhere/else/b7a4/id1",
                                       payload: "2|opus|1|2|0|h"))
    }

    func testExtraTopicSegmentsRejected() {
        XCTAssertNil(MQTTMessage.parse(topic: "iotj/cl/openwr/updates/b7a4/id1/extra",
                                       payload: "2|opus|1|2|0|h"))
    }
}
