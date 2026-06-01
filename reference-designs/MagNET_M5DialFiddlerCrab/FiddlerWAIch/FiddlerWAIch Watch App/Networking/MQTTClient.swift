import Foundation
import Combine

/// Subscribe-only MQTT 3.1.1 client over WebSocket using URLSessionWebSocketTask.
///
/// Default transport on watchOS: URLSession traffic is tunnelled through the paired iPhone's
/// Companion Link automatically, so this works even when the watch has no direct Wi-Fi.
/// See `MQTTClientTCP` for an alternative raw-TCP implementation that requires direct Wi-Fi.
@MainActor
final class MQTTClient: NSObject, ObservableObject, URLSessionDelegate, URLSessionWebSocketDelegate {
    @Published private(set) var connection: ConnectionState = .disconnected
    let incoming = PassthroughSubject<MQTTMessage, Never>()

    private let settings: AppSettings
    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    private var currentMac4: String = ""

    private var reconnectTask: Task<Void, Never>?
    private var reconnectAttempt: Int = 0

    private var pingTimer: Timer?
    private var rxBuffer = Data()
    private var subscribePacketId: UInt16 = 1
    private var isIntentionallyDisconnecting = false

    init(settings: AppSettings) {
        self.settings = settings
        super.init()
    }

    func connect() {
        guard !settings.mac4.isEmpty else {
            MQTTLog.shared.append("[MQTT-WS] connect skipped: mac4 empty")
            return
        }
        // Re-entry guard — don't tear down an in-flight connection attempt.
        // Previous behavior: every .onChange(of: scenePhase) + reconnect fire would cancel
        // the previous task, causing a cascade of "Cancelled" errors that prevented any
        // connection from completing.
        if connection == .connecting || connection == .connected {
            MQTTLog.shared.append("[MQTT-WS] connect skipped: already \(connection)")
            return
        }
        guard let url = URL(string: settings.activeBrokerURL) else {
            MQTTLog.shared.append("[MQTT-WS] connect skipped: bad URL \"\(settings.activeBrokerURL)\"")
            return
        }
        MQTTLog.shared.append("[MQTT-WS] connecting → \(url.absoluteString) mac4=\(settings.mac4) preset=\(settings.brokerPreset.rawValue)")

        settings.ensureClientId()
        currentMac4 = settings.mac4.lowercased()
        cancelReconnect()
        teardownSocket()
        connection = .connecting

        let config = URLSessionConfiguration.default
        config.waitsForConnectivity = false // fail fast if no path, let our reconnect logic handle
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 120
        let session = URLSession(configuration: config, delegate: self, delegateQueue: .main)
        let task = session.webSocketTask(with: url, protocols: ["mqtt", "mqttv3.1"])
        self.session = session
        self.task = task
        task.resume()
    }

    func disconnect() {
        isIntentionallyDisconnecting = true
        cancelReconnect()
        sendRaw(MQTTPacket.disconnect())
        teardownSocket()
        connection = .disconnected
        isIntentionallyDisconnecting = false
    }

    func resubscribe() {
        MQTTLog.shared.append("[MQTT-WS] resubscribe (broker changed)")
        disconnect()
        connect()
    }

    // MARK: - URLSessionWebSocketDelegate

    nonisolated func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
        Task { @MainActor in
            MQTTLog.shared.append("[MQTT-WS] WebSocket open (protocol=\(`protocol` ?? "nil"))")
            self.onWebSocketOpen()
        }
    }

    nonisolated func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        Task { @MainActor in
            MQTTLog.shared.append("[MQTT-WS] WebSocket close code=\(closeCode.rawValue)")
            self.onWebSocketClose()
        }
    }

    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        Task { @MainActor in
            if let error {
                MQTTLog.shared.append("[MQTT-WS] task error: \(error.localizedDescription)")
            }
            self.onWebSocketClose()
        }
    }

    // MARK: - Socket lifecycle

    private func onWebSocketOpen() {
        sendRaw(MQTTPacket.connect(clientId: settings.clientId, keepAlive: MQTTConfig.keepAliveSeconds))
        receiveNext()
    }

    private func onWebSocketClose() {
        if isIntentionallyDisconnecting {
            MQTTLog.shared.append("[MQTT-WS] close during intentional disconnect — not scheduling reconnect")
            return
        }
        guard connection != .disconnected else { return }
        connection = .disconnected
        teardownSocket()
        scheduleReconnect()
    }

    private func teardownSocket() {
        pingTimer?.invalidate()
        pingTimer = nil
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
        session?.invalidateAndCancel()
        session = nil
        rxBuffer.removeAll()
    }

    // MARK: - Reconnect

    private func scheduleReconnect() {
        cancelReconnect()
        let delays: [UInt64] = [1, 2, 4, 8, 16, 30]
        let delay = delays[min(reconnectAttempt, delays.count - 1)]
        reconnectAttempt += 1
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

    // MARK: - MQTT

    private func sendRaw(_ data: Data) {
        task?.send(.data(data)) { _ in }
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
        task?.receive { [weak self] result in
            Task { @MainActor [weak self] in
                guard let self else { return }
                switch result {
                case .failure:
                    self.onWebSocketClose()
                case .success(let msg):
                    switch msg {
                    case .data(let d):    self.handleInbound(d)
                    case .string(let s):  self.handleInbound(Data(s.utf8))
                    @unknown default:     break
                    }
                    self.receiveNext()
                }
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

    /// Peel the next complete MQTT packet off a buffer. Returns (packetBytes, consumedBytes) or nil if incomplete.
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
                onWebSocketClose()
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
}
