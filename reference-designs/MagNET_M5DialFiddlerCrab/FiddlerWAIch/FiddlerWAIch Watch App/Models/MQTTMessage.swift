import Foundation

struct MQTTMessage: Equatable {
    let mac4: String
    let sessionId: String
    let state: SessionState
    let model: String
    let sessionPct: Int
    let weeklyPct: Int
    let resetEpoch: UInt64
    let clientHost: String
    let promptPreview: String

    /// Parse a pipe-delimited payload and MQTT topic.
    /// Topic format: `iotj/cl/openwr/updates/<mac4>/<sessionId>`
    /// Payload (6-field legacy): `state|model|session_pct|weekly_pct|reset_epoch|client_host`
    /// Payload (7-field extended): `…|prompt_preview`
    /// Returns nil on malformed input.
    static func parse(topic: String, payload: String) -> MQTTMessage? {
        let topicParts = topic.split(separator: "/", omittingEmptySubsequences: false)
        guard topicParts.count >= 2 else { return nil }

        let mac4 = String(topicParts[topicParts.count - 2])
        let sessionId = String(topicParts[topicParts.count - 1])
        guard !mac4.isEmpty, !sessionId.isEmpty else { return nil }

        let fields = payload.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard fields.count >= 6 else { return nil }

        guard let rawState = Int(fields[0]) else { return nil }
        let model = fields[1]
        let sessionPct = Int(fields[2]) ?? -1
        let weeklyPct = Int(fields[3]) ?? -1
        let resetEpoch = UInt64(fields[4]) ?? 0
        let clientHost = fields[5]
        let promptPreview = fields.count >= 7 ? fields[6] : ""

        return MQTTMessage(
            mac4: mac4,
            sessionId: sessionId,
            state: SessionState(raw: rawState),
            model: model,
            sessionPct: sessionPct,
            weeklyPct: weeklyPct,
            resetEpoch: resetEpoch,
            clientHost: clientHost,
            promptPreview: promptPreview
        )
    }
}
