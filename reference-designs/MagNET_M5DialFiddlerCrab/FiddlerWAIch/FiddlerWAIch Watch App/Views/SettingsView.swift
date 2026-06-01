import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var settings: AppSettings
    @EnvironmentObject var mqtt: MQTTClient
    @EnvironmentObject var netmon: NetworkMonitor
    @Environment(\.dismiss) private var dismiss
    @State private var mac4Draft: String = ""
    @State private var showPublicWarning = false
    @State private var probeResult: String = "未実行"
    @State private var probing: Bool = false
    @ObservedObject private var mqttLog = MQTTLog.shared

    var body: some View {
        ZStack {
            SynthwaveBackground()
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    Text("settings.title")
                        .font(.system(size: 20, weight: .black))
                        .foregroundStyle(Theme.cyan)
                        .shadow(color: Theme.cyan.opacity(0.8), radius: 3)

                    section(titleKey: "settings.device_id") {
                        TextField("", text: $mac4Draft)
                            .multilineTextAlignment(.center)
                            .font(.system(size: 22, weight: .heavy, design: .monospaced))
                            .foregroundStyle(Theme.textBright)
                            .frame(height: 40)
                            .background(Theme.bgMid)
                            .overlay(
                                RoundedRectangle(cornerRadius: 6).stroke(Theme.hotPink, lineWidth: 1.5)
                            )
                            .onAppear { mac4Draft = settings.mac4 }
                        Button {
                            let trimmed = AppSettings.sanitizeMac4(mac4Draft)
                            if AppSettings.isValidMac4(trimmed) {
                                settings.mac4 = trimmed
                                mqtt.resubscribe()
                            }
                        } label: {
                            Text("onboarding.save")
                                .font(.system(size: 15, weight: .bold))
                        }
                        .tint(Theme.hotPink)
                        .disabled(!AppSettings.isValidMac4(AppSettings.sanitizeMac4(mac4Draft)))
                    }

                    section(titleKey: "settings.broker") {
                        ForEach(BrokerPreset.allCases) { preset in
                            Button {
                                select(preset: preset)
                            } label: {
                                HStack {
                                    Image(systemName: settings.brokerPreset == preset ? "circle.inset.filled" : "circle")
                                        .foregroundStyle(Theme.neonGreen)
                                    Text(preset.displayKey)
                                        .font(.system(size: 14, weight: .bold))
                                        .foregroundStyle(Theme.textBright)
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }

                    section(titleKey: "settings.feedback") {
                        ForEach(FeedbackMode.allCases) { mode in
                            Button {
                                settings.feedbackMode = mode
                            } label: {
                                HStack {
                                    Image(systemName: settings.feedbackMode == mode ? "circle.inset.filled" : "circle")
                                        .foregroundStyle(Theme.neonGreen)
                                    Text(mode.displayKey)
                                        .font(.system(size: 14, weight: .bold))
                                        .foregroundStyle(Theme.textBright)
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Text("DEBUG  build \(AppSettings.buildTimestamp)")
                            .font(.system(size: 10, weight: .black, design: .monospaced))
                            .foregroundStyle(Theme.neonOrange)
                        // Buttons first so no scrolling is needed.
                        Button {
                            mqtt.resubscribe()
                        } label: {
                            Text("再接続 FORCE")
                                .font(.system(size: 12, weight: .black, design: .monospaced))
                                .frame(maxWidth: .infinity)
                        }
                        .tint(Theme.cyan)
                        Button {
                            runProbe()
                        } label: {
                            Text(probing ? "テスト中…" : "接続テスト")
                                .font(.system(size: 12, weight: .black, design: .monospaced))
                                .frame(maxWidth: .infinity)
                        }
                        .tint(Theme.neonGreen)
                        .disabled(probing)
                        // Live status lines.
                        Text("conn: \(connectionLabel(mqtt.connection))")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(connectionColor(mqtt.connection))
                        Text("net: \(netmon.pathStatus) / \(netmon.interface)")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(netmon.isReachable ? Theme.neonGreen : Theme.red)
                        Text("ip: \(IPInfo.summary)")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(Theme.textBright)
                        Text("probe: \(probeResult)")
                            .font(.system(size: 10, weight: .bold, design: .monospaced))
                            .foregroundStyle(Theme.textBright)
                            .lineLimit(3)
                        Text("mac4: \(settings.mac4.isEmpty ? "—" : settings.mac4)")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(Theme.textBright)
                        Text("preset: \(settings.brokerPreset.rawValue)")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(Theme.textBright)
                        Text("url: \(settings.activeBrokerURL)")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(Theme.textBright)
                            .lineLimit(2)
                        Divider().background(Theme.neonOrange)
                        Text("LOG (最新)")
                            .font(.system(size: 10, weight: .black, design: .monospaced))
                            .foregroundStyle(Theme.neonOrange)
                        if mqttLog.entries.isEmpty {
                            Text("(まだ無し)")
                                .font(.system(size: 10, weight: .bold, design: .monospaced))
                                .foregroundStyle(Theme.textDim)
                        } else {
                            ForEach(Array(mqttLog.entries.suffix(6).enumerated()), id: \.offset) { _, line in
                                Text(line)
                                    .font(.system(size: 9, weight: .medium, design: .monospaced))
                                    .foregroundStyle(Theme.textBright)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Theme.bgMid)
                    .overlay(
                        RoundedRectangle(cornerRadius: 6).stroke(Theme.neonOrange, lineWidth: 1)
                    )

                    Button {
                        dismiss()
                    } label: {
                        Text("settings.close")
                            .font(.system(size: 15, weight: .bold))
                            .frame(maxWidth: .infinity)
                    }
                    .tint(Theme.cyan)
                }
                .padding()
            }
        }
        .alert("settings.public_broker_warning_title", isPresented: $showPublicWarning) {
            Button("settings.public_broker_warning_confirm") {
                settings.publicBrokerAcknowledged = true
                settings.brokerPreset = .hivemqPublic
                mqtt.resubscribe()
            }
            Button("settings.cancel", role: .cancel) {}
        } message: {
            Text("settings.public_broker_warning_body")
        }
    }

    @ViewBuilder
    private func section<Content: View>(titleKey: LocalizedStringKey, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(titleKey)
                .font(.system(size: 13, weight: .black))
                .foregroundStyle(Theme.hotPink)
                .shadow(color: Theme.hotPink.opacity(0.7), radius: 2)
            content()
        }
        .padding(.vertical, 4)
    }

    private func connectionLabel(_ s: ConnectionState) -> String {
        switch s {
        case .connected:    return "CONNECTED"
        case .connecting:   return "connecting…"
        case .disconnected: return "DISCONNECTED"
        }
    }

    private func connectionColor(_ s: ConnectionState) -> Color {
        switch s {
        case .connected:    return Theme.neonGreen
        case .connecting:   return Theme.yellow
        case .disconnected: return Theme.red
        }
    }

    private func select(preset: BrokerPreset) {
        if preset == .hivemqPublic && !settings.publicBrokerAcknowledged {
            showPublicWarning = true
            return
        }
        settings.brokerPreset = preset
        mqtt.resubscribe()
    }

    private func runProbe() {
        probing = true
        probeResult = "実行中"
        Task {
            async let apple = probe(url: "http://captive.apple.com/hotspot-detect.html", label: "http")
            async let hivemq = probe(url: "http://broker.hivemq.com:8000/mqtt", label: "hivemq-get")
            async let wsTest = probeWebSocket(url: "ws://broker.hivemq.com:8000/mqtt", label: "ws")
            let results = await [apple, hivemq, wsTest]
            await MainActor.run {
                probeResult = results.joined(separator: "\n")
                probing = false
            }
        }
    }

    private nonisolated func probeWebSocket(url: String, label: String) async -> String {
        guard let u = URL(string: url) else { return "\(label):bad-url" }
        return await withCheckedContinuation { (cont: CheckedContinuation<String, Never>) in
            let task = URLSession.shared.webSocketTask(with: u, protocols: ["mqtt"])
            task.resume()
            // Try to receive — first receive triggers upgrade handshake.
            task.receive { result in
                switch result {
                case .success:
                    task.cancel(with: .normalClosure, reason: nil)
                    cont.resume(returning: "\(label):OK")
                case .failure(let err):
                    let ns = err as NSError
                    cont.resume(returning: "\(label):err\(ns.code)")
                }
            }
            // Backup timeout
            DispatchQueue.global().asyncAfter(deadline: .now() + 8) {
                task.cancel(with: .abnormalClosure, reason: nil)
            }
        }
    }

    private nonisolated func probe(url: String, label: String) async -> String {
        guard let u = URL(string: url) else { return "\(label):bad-url" }
        var req = URLRequest(url: u)
        req.timeoutInterval = 8
        do {
            let (_, resp) = try await URLSession.shared.data(for: req)
            if let http = resp as? HTTPURLResponse {
                return "\(label):\(http.statusCode)"
            }
            return "\(label):?"
        } catch {
            let ns = error as NSError
            return "\(label):err\(ns.code)"
        }
    }
}
