import SwiftUI

struct ConnectionDot: View {
    let state: ConnectionState

    var body: some View {
        HStack(spacing: 5) {
            Rectangle()
                .fill(dotColor)
                .frame(width: 10, height: 10)
                .shadow(color: dotColor.opacity(0.9), radius: 4)
                .opacity(opacity)
                .animation(state == .connecting ? .easeInOut(duration: 0.8).repeatForever(autoreverses: true) : .default, value: state)
            Text(label)
                .font(.system(size: 15, weight: .black))
                .foregroundStyle(Theme.textDim)
        }
    }

    private var dotColor: Color {
        switch state {
        case .connected:    return Theme.neonGreen
        case .connecting:   return Theme.yellow
        case .disconnected: return Theme.red
        }
    }

    private var opacity: Double {
        state == .connecting ? 0.5 : 1.0
    }

    private var label: LocalizedStringKey {
        switch state {
        case .connected:    return "conn.connected"
        case .connecting:   return "conn.connecting"
        case .disconnected: return "conn.disconnected"
        }
    }
}
