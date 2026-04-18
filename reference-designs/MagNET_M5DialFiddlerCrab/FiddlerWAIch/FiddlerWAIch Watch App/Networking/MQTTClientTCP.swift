import Foundation
import Combine
import Network

/// Alternative: Subscribe-only MQTT 3.1.1 client over raw TCP using NWConnection.
/// Matches the transport used by the M5StickC Plus and M5Dial reference designs.
///
/// **Caveat:** NWConnection on watchOS *does not* tunnel through the iPhone Companion Link for
/// third-party apps. This class only works when the watch has a direct internet path (its own
/// Wi-Fi or cellular). If the watch is only reachable via iPhone Bluetooth relay, use the
/// URLSession-based `MQTTClient` instead — URLSession traffic is tunnelled automatically.
///
/// URL format: `mqtt://host:port` (non-TLS) or `mqtts://host:port` (TLS).
/// Falls back to port 1883 / 8883 if port omitted.
@MainActor
final class MQTTClientTCP: ObservableObject {
    @Published private(set) var connection: ConnectionState = .disconnected
    let incoming = PassthroughSubject<MQTTMessage, Never>()

    private let settings: AppSettings
    private var conn: NWConnection?
    private var currentMac4: String = ""
    private var reconnectTask: Task<Void, Never>?
    private var reconnectAttempt: Int = 0
    private var pingTimer: Timer?
    private var rxBuffer = Data()
    private var subscribePacketId: UInt16 = 1

    init(settings: AppSettings) {
        self.settings = settings
    }

    func connect() {
        guard !settings.mac4.isEmpty else {
            print("[MQTT] connect skipped: mac4 empty")
            return
        }
        let urlStr = settings.activeBrokerURL
        guard let (host, port, useTLS) = parseBrokerURL(urlStr) else {
            print("[MQTT] connect skipped: cannot parse URL \"\(urlStr)\"")
            return
        }
        print("[MQTT] connecting → host=\(host) port=\(port) tls=\(useTLS) mac4=\(settings.mac4) preset=\(settings.brokerPreset.rawValue)")

        settings.ensureClientId()
        currentMac4 = settings.mac4.lowercased()
        cancelReconnect()
        teardown()
        connection = .connecting

        let params: NWParameters = useTLS ? .tls : .tcp
        params.includePeerToPeer = false
        params.allowLocalEndpointReuse = true
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(integerLiteral: port))
        let c = NWConnection(to: endpoint, using: params)
        c.stateUpdateHandler = { [weak self] state in
            Task { @MainActor [weak self] in self?.onNWStateChange(state) }
        }
        self.conn = c
        c.start(queue: .main)
    }

    func disconnect() {
        cancelReconnect()
        sendRaw(MQTTPacket.disconnect())
        teardown()
        connection = .disconnected
    }

    func resubscribe() {
        disconnect()
        connect()
    }

    // MARK: - NWConnection state

    private func onNWStateChange(_ state: NWConnection.State) {
        print("[MQTT] NWConnection state → \(describe(state))")
        switch state {
        case .ready:
            sendRaw(MQTTPacket.connect(clientId: settings.clientId, keepAlive: MQTTConfig.keepAliveSeconds))
            receiveNext()
        case .failed(let err):
            print("[MQTT] failed: \(err)")
            if connection != .disconnected {
                connection = .disconnected
                teardown()
                scheduleReconnect()
            }
        case .cancelled:
            if connection != .disconnected {
                connection = .disconnected
                teardown()
                scheduleReconnect()
            }
        case .waiting(let err):
            // NWConnection intentionally stays in .waiting while path is unsatisfied —
            // it resumes on its own when the network comes back. Don't force reconnect.
            // Only bail out if we've been waiting a long time (90s) for slow-connection users.
            print("[MQTT] waiting: \(err)")
            scheduleWaitTimeout(seconds: 90)
        default:
            break
        }
    }

    private var waitTimeoutTask: Task<Void, Never>?
    private func scheduleWaitTimeout(seconds: UInt64) {
        waitTimeoutTask?.cancel()
        waitTimeoutTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: seconds * 1_000_000_000)
            guard let self, !Task.isCancelled else { return }
            if self.connection != .connected {
                print("[MQTT] wait timeout after \(seconds)s — forcing reconnect")
                self.teardown()
                self.connection = .disconnected
                self.scheduleReconnect()
            }
        }
    }

    private func describe(_ s: NWConnection.State) -> String {
        switch s {
        case .setup:       return "setup"
        case .waiting(let e): return "waiting(\(e))"
        case .preparing:   return "preparing"
        case .ready:       return "ready"
        case .failed(let e): return "failed(\(e))"
        case .cancelled:   return "cancelled"
        @unknown default:  return "?"
        }
    }

    private func teardown() {
        pingTimer?.invalidate()
        pingTimer = nil
        waitTimeoutTask?.cancel()
        waitTimeoutTask = nil
        conn?.cancel()
        conn = nil
        rxBuffer.removeAll()
    }

    // MARK: - Reconnect

    private func scheduleReconnect() {
        cancelReconnect()
        let delays: [UInt64] = [3, 6, 12, 24, 48, 60]
        let delay = delays[min(reconnectAttempt, delays.count - 1)]
        reconnectAttempt += 1
        print("[MQTT] reconnect in \(delay)s (attempt \(reconnectAttempt))")
        reconnectTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: delay * 1_000_000_000)
            guard let self, !Task.isCancelled else { return }
            self.connect()
        }
    }

    private func cancelReconnect() {
        reconnectTask?.cancel()
        reconnectTask = nil
        reconnectAttempt = 0
    }

    // MARK: - MQTT framing over NWConnection

    private func sendRaw(_ data: Data) {
        conn?.send(content: data, completion: .contentProcessed { _ in })
    }

    private func subscribeTopic() {
        let topic = MQTTConfig.topicPattern(mac4: currentMac4)
        subscribePacketId = subscribePacketId &+ 1
        sendRaw(MQTTPacket.subscribe(topic: topic, qos: 1, packetId: subscribePacketId))
    }

    private func startPingTimer() {
        pingTimer?.invalidate()
        let keepAlive = TimeInterval(MQTTConfig.keepAliveSeconds) * 0.8
        let t = Timer(timeInterval: keepAlive, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.sendRaw(MQTTPacket.pingreq()) }
        }
        RunLoop.main.add(t, forMode: .common)
        pingTimer = t
    }

    private func receiveNext() {
        conn?.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if let data, !data.isEmpty {
                    self.handleInbound(data)
                }
                if error != nil || isComplete {
                    self.onNWStateChange(.cancelled)
                    return
                }
                self.receiveNext()
            }
        }
    }

    private func handleInbound(_ data: Data) {
        rxBuffer.append(data)
        while let (packet, consumed) = frameNextPacket(from: rxBuffer) {
            rxBuffer.removeFirst(consumed)
            processPacket(packet)
        }
    }

    private func frameNextPacket(from buf: Data) -> (Data, Int)? {
        guard buf.count >= 2 else { return nil }
        var idx = 1
        var multiplier = 1
        var remaining = 0
        var loop = 0
        while idx < buf.count {
            let b = buf[idx]
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
        return (buf.prefix(total), total)
    }

    private func processPacket(_ data: Data) {
        guard let decoded = MQTTPacket.decode(data) else { return }
        switch decoded {
        case .connack(let success):
            if success {
                connection = .connected
                reconnectAttempt = 0
                subscribeTopic()
                startPingTimer()
            } else {
                onNWStateChange(.cancelled)
            }
        case .suback:
            break
        case .publish(let topic, let payload):
            if let payloadStr = String(data: payload, encoding: .utf8),
               let msg = MQTTMessage.parse(topic: topic, payload: payloadStr) {
                incoming.send(msg)
            }
        case .pingresp, .other:
            break
        }
    }

    // MARK: - URL parsing

    /// Accepts: `mqtt://host[:port]`, `mqtts://host[:port]`,
    /// and (for convenience) `ws://`, `wss://`, `tcp://` treated as plain TCP with their own default ports.
    private func parseBrokerURL(_ s: String) -> (host: String, port: UInt16, tls: Bool)? {
        guard let comps = URLComponents(string: s), let host = comps.host, !host.isEmpty else { return nil }
        let scheme = (comps.scheme ?? "mqtt").lowercased()
        let tls = (scheme == "mqtts" || scheme == "wss" || scheme == "ssl")
        let defaultPort: UInt16 = tls ? 8883 : 1883
        let port = UInt16(comps.port ?? Int(defaultPort))
        return (host, port, tls)
    }
}
