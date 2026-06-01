import SwiftUI

struct SessionPagerView: View {
    @EnvironmentObject var store: SessionStore
    @EnvironmentObject var mqtt: MQTTClient
    @EnvironmentObject var feedback: FeedbackManager
    @State private var showSettings = false

    var body: some View {
        ZStack(alignment: .topTrailing) {
            content

            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(Theme.cyan)
                    .shadow(color: Theme.cyan.opacity(0.8), radius: 3)
                    .padding(6)
                    .background(
                        Circle()
                            .fill(Theme.bgMid.opacity(0.85))
                            .overlay(Circle().stroke(Theme.hotPink, lineWidth: 1))
                    )
            }
            .buttonStyle(.plain)
            .padding(.top, 4)
            .padding(.trailing, 6)
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
        }
    }

    @ViewBuilder
    private var content: some View {
        if store.sessions.isEmpty {
            EmptyStateView(connection: mqtt.connection)
        } else {
            TabView(selection: $store.currentIndex) {
                ForEach(Array(store.sessions.enumerated()), id: \.element.id) { idx, session in
                    SessionView(
                        session: session,
                        index: idx,
                        total: store.sessions.count,
                        connection: mqtt.connection
                    )
                    .tag(idx)
                }
            }
            .tabViewStyle(.page(indexDisplayMode: .never))
            .onChange(of: store.currentIndex) { _ in
                feedback.swipeClick()
            }
        }
    }
}
