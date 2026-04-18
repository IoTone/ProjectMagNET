import SwiftUI

@main
struct FiddlerWAIchApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var settings = AppSettings()
    @StateObject private var feedback: FeedbackManager
    @StateObject private var store: SessionStore
    @StateObject private var mqtt: MQTTClient
    @StateObject private var netmon = NetworkMonitor()

    init() {
        let settings = AppSettings()
        let feedback = FeedbackManager(settings: settings)
        let store = SessionStore(feedback: feedback)
        let mqtt = MQTTClient(settings: settings)
        _settings = StateObject(wrappedValue: settings)
        _feedback = StateObject(wrappedValue: feedback)
        _store = StateObject(wrappedValue: store)
        _mqtt = StateObject(wrappedValue: mqtt)
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(settings)
                .environmentObject(feedback)
                .environmentObject(store)
                .environmentObject(mqtt)
                .environmentObject(netmon)
                .onAppear {
                    wireMQTT()
                }
                .onReceive(mqtt.incoming) { msg in
                    store.ingest(msg)
                }
                .onReceive(mqtt.$connection) { state in
                    switch state {
                    case .connected:    feedback.connected()
                    case .disconnected: feedback.disconnected()
                    case .connecting:   break
                    }
                }
        }
        .onChange(of: scenePhase) { phase in
            switch phase {
            case .active:
                mqtt.connect()
            case .background, .inactive:
                mqtt.disconnect()
            @unknown default:
                break
            }
        }
    }

    private func wireMQTT() {
        if !settings.mac4.isEmpty {
            mqtt.connect()
        }
    }
}
