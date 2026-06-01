import SwiftUI

struct SessionView: View {
    let session: Session
    let index: Int
    let total: Int
    let connection: ConnectionState

    var body: some View {
        ZStack {
            SynthwaveBackground()

            RadialRingsView(
                workingElapsed: session.workingElapsed,
                sessionPct: session.sessionPct,
                sessionColor: Theme.sessionColor(index: session.colorIndex),
                state: session.state,
                isStale: session.isStale
            )
            .ignoresSafeArea()

            VStack(spacing: 6) {
                header
                stateLine
                accentBar
                PromptPreview(text: session.promptPreview)
                    .frame(maxHeight: 58)
                CrawdadView(
                    state: session.state,
                    sessionColor: Theme.sessionColor(index: session.colorIndex),
                    dimmed: session.isStale
                )
                .frame(width: 96, height: 72)
                footer
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 6)
            .opacity(session.isStale ? 0.7 : 1.0)
        }
    }

    private var header: some View {
        HStack {
            IndexBadge(current: index, total: total)
            Spacer()
            ConnectionDot(state: connection)
        }
    }

    private var stateLine: some View {
        HStack(spacing: 6) {
            Text(session.model)
                .font(.system(size: 19, weight: .heavy, design: .monospaced))
                .foregroundStyle(Theme.neonGreen)
                .shadow(color: Theme.neonGreen.opacity(0.7), radius: 2)
            Text(session.state.localizedKey)
                .font(.system(size: 19, weight: .heavy))
                .foregroundStyle(session.state.accentColor)
                .shadow(color: session.state.accentColor.opacity(0.8), radius: 3)
            Spacer()
            if session.state == .working {
                Text(timerString(session.workingElapsed))
                    .font(.system(size: 19, weight: .heavy, design: .monospaced))
                    .foregroundStyle(Theme.textBright)
                    .shadow(color: Theme.cyan.opacity(0.6), radius: 2)
            }
        }
    }

    private var accentBar: some View {
        Rectangle()
            .fill(Theme.sessionColor(index: session.colorIndex))
            .frame(height: 3)
            .shadow(color: Theme.sessionColor(index: session.colorIndex).opacity(0.9), radius: 4)
    }

    private var footer: some View {
        HStack {
            Text(session.sessionPct >= 0 ? "\(session.sessionPct)%" : "—")
                .foregroundStyle(Theme.cyan)
            Spacer()
            Text(session.mac4)
                .foregroundStyle(Theme.textDim)
            Spacer()
            Text(session.clientHost.isEmpty ? "—" : session.clientHost)
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(Theme.textDim)
        }
        .font(.system(size: 15, weight: .bold, design: .monospaced))
    }

    private func timerString(_ elapsed: TimeInterval) -> String {
        let total = Int(elapsed)
        return String(format: "%02d:%02d", total / 60, total % 60)
    }
}
