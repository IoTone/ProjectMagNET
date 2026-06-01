import SwiftUI

struct EmptyStateView: View {
    let connection: ConnectionState

    var body: some View {
        ZStack {
            SynthwaveBackground()
            VStack(spacing: 10) {
                Spacer()
                CrawdadView(state: .idle, sessionColor: Theme.cyan, dimmed: false)
                    .frame(width: 120, height: 84)
                Text("empty.no_sessions")
                    .font(.system(size: 18, weight: .black, design: .monospaced))
                    .foregroundStyle(Theme.textDim)
                    .shadow(color: Theme.hotPink.opacity(0.6), radius: 3)
                Spacer()
                ConnectionDot(state: connection)
                Text("brand.tagline")
                    .font(.system(size: 12, weight: .bold))
                    .foregroundStyle(Theme.cyan.opacity(0.8))
                Text("build \(AppSettings.buildTimestamp)")
                    .font(.system(size: 9, weight: .medium, design: .monospaced))
                    .foregroundStyle(Theme.neonOrange.opacity(0.9))
                    .padding(.bottom, 4)
            }
        }
    }
}
