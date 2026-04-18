import SwiftUI

/// Dual concentric rings hugging the watch safe-area perimeter.
/// Outer: session color, sweeps 0-360° per hour of working time.
/// Inner: token consumption %, NEON_GREEN → YELLOW → RED gradient.
struct RadialRingsView: View {
    let workingElapsed: TimeInterval
    let sessionPct: Int              // -1 hides inner ring
    let sessionColor: Color
    let state: SessionState
    let isStale: Bool

    private let outerInset: CGFloat = 2
    private let outerWidth: CGFloat = 4
    private let innerGap: CGFloat = 4
    private let innerWidth: CGFloat = 3

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0, paused: false)) { _ in
            Canvas { ctx, size in
                drawOuter(ctx: ctx, size: size)
                drawInner(ctx: ctx, size: size)
            }
            .allowsHitTesting(false)
        }
    }

    private func cornerRadius(for size: CGSize) -> CGFloat {
        min(size.width, size.height) * 0.35
    }

    private func roundedRectPath(insetBy inset: CGFloat, in size: CGSize) -> Path {
        let rect = CGRect(x: inset, y: inset,
                          width: size.width - inset * 2,
                          height: size.height - inset * 2)
        let radius = max(0, cornerRadius(for: size) - inset)
        return Path(roundedRect: rect, cornerRadius: radius)
    }

    private func drawOuter(ctx: GraphicsContext, size: CGSize) {
        let full = roundedRectPath(insetBy: outerInset, in: size)
        // faint track
        ctx.stroke(full, with: .color(sessionColor.opacity(0.15)), lineWidth: outerWidth)

        let fraction = fractionForHourlyWrap(workingElapsed)
        let progressPath = partialPath(full: full, fraction: fraction)
        let color = resolveOuterColor()
        ctx.stroke(progressPath, with: .color(color), style: StrokeStyle(lineWidth: outerWidth, lineCap: .round))
    }

    private func drawInner(ctx: GraphicsContext, size: CGSize) {
        guard sessionPct >= 0 else { return }
        let inset = outerInset + outerWidth + innerGap
        let full = roundedRectPath(insetBy: inset, in: size)
        ctx.stroke(full, with: .color(Theme.dimGray.opacity(0.3)), lineWidth: innerWidth)

        let fraction = min(1.0, Double(sessionPct) / 100.0)
        let progressPath = partialPath(full: full, fraction: fraction)
        let color = tokenColor(pct: sessionPct)
        ctx.stroke(progressPath, with: .color(color), style: StrokeStyle(lineWidth: innerWidth, lineCap: .round))
    }

    private func partialPath(full: Path, fraction: Double) -> Path {
        guard fraction > 0 else { return Path() }
        let clamped = min(max(fraction, 0), 1)
        return full.trimmedPath(from: 0, to: clamped)
    }

    private func fractionForHourlyWrap(_ elapsed: TimeInterval) -> Double {
        let mod = elapsed.truncatingRemainder(dividingBy: 3600)
        return mod / 3600.0
    }

    private func resolveOuterColor() -> Color {
        if isStale { return sessionColor.opacity(0.2) }
        if state != .working { return sessionColor.opacity(0.5) }
        return sessionColor
    }

    private func tokenColor(pct: Int) -> Color {
        let p = Double(max(0, min(100, pct))) / 100.0
        if p < 0.5 {
            return Theme.neonGreen.blended(with: Theme.yellow, t: p / 0.5)
        } else {
            return Theme.yellow.blended(with: Theme.red, t: (p - 0.5) / 0.5)
        }
    }
}

private extension Color {
    func blended(with other: Color, t: Double) -> Color {
        // Approximate RGB blend via UIKit intermediate (watchOS has UIColor).
        let t = min(max(t, 0), 1)
        #if canImport(UIKit)
        let a = UIColor(self)
        let b = UIColor(other)
        var r1: CGFloat = 0, g1: CGFloat = 0, b1: CGFloat = 0, a1: CGFloat = 0
        var r2: CGFloat = 0, g2: CGFloat = 0, b2: CGFloat = 0, a2: CGFloat = 0
        a.getRed(&r1, green: &g1, blue: &b1, alpha: &a1)
        b.getRed(&r2, green: &g2, blue: &b2, alpha: &a2)
        return Color(
            red:   Double(r1 + (r2 - r1) * CGFloat(t)),
            green: Double(g1 + (g2 - g1) * CGFloat(t)),
            blue:  Double(b1 + (b2 - b1) * CGFloat(t))
        )
        #else
        return t < 0.5 ? self : other
        #endif
    }
}
