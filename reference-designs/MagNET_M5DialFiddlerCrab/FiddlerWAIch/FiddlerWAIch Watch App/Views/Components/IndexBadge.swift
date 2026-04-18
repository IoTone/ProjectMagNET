import SwiftUI

struct IndexBadge: View {
    let current: Int
    let total: Int

    var body: some View {
        Text(total == 0 ? "" : "\(current + 1)/\(total)")
            .font(.system(size: 20, weight: .black, design: .monospaced))
            .foregroundStyle(Theme.cyan)
            .shadow(color: Theme.cyan.opacity(0.85), radius: 3)
    }
}
