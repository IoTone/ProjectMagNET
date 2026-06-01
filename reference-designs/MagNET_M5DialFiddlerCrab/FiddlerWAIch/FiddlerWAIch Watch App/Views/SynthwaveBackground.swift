import SwiftUI

/// Outrun-style backdrop: dark-purple sky above a horizon with receding neon grid + sun glow.
/// Kept subtle so session content stays legible.
struct SynthwaveBackground: View {
    var body: some View {
        TimelineView(.animation(minimumInterval: 0.1, paused: false)) { ctx in
            Canvas { g, size in
                drawSky(g: g, size: size)
                drawSun(g: g, size: size)
                drawHorizon(g: g, size: size)
                drawGrid(g: g, size: size, time: ctx.date.timeIntervalSince1970)
                drawScanlines(g: g, size: size)
            }
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
    }

    private func drawSky(g: GraphicsContext, size: CGSize) {
        let rect = CGRect(origin: .zero, size: size)
        g.fill(Path(rect), with: .linearGradient(
            Gradient(colors: [Theme.bg, Theme.bgMid, Theme.bg]),
            startPoint: CGPoint(x: size.width / 2, y: 0),
            endPoint: CGPoint(x: size.width / 2, y: size.height)
        ))
    }

    private func drawSun(g: GraphicsContext, size: CGSize) {
        // Sun-like glow centered on horizon (~65% down)
        let horizonY = size.height * 0.62
        let cx = size.width / 2
        let r = size.width * 0.40
        let rect = CGRect(x: cx - r, y: horizonY - r, width: r * 2, height: r * 2)
        g.fill(Path(ellipseIn: rect), with: .radialGradient(
            Gradient(colors: [Theme.hotPink.opacity(0.35), Theme.neonOrange.opacity(0.15), .clear]),
            center: CGPoint(x: cx, y: horizonY),
            startRadius: 0,
            endRadius: r
        ))
    }

    private func drawHorizon(g: GraphicsContext, size: CGSize) {
        let horizonY = size.height * 0.62
        var path = Path()
        path.move(to: CGPoint(x: 0, y: horizonY))
        path.addLine(to: CGPoint(x: size.width, y: horizonY))
        g.stroke(path, with: .color(Theme.gridPink.opacity(0.5)), lineWidth: 1)
    }

    private func drawGrid(g: GraphicsContext, size: CGSize, time: TimeInterval) {
        let horizonY = size.height * 0.62
        let cx = size.width / 2
        let vanishX = cx
        let bottom = size.height + 4

        // Perspective converging verticals (radial from vanishing point)
        let verticalCount = 11
        for i in 0..<verticalCount {
            let t = CGFloat(i) / CGFloat(verticalCount - 1)   // 0..1
            let spread = size.width * 1.4
            let bottomX = (t - 0.5) * spread + cx
            var p = Path()
            p.move(to: CGPoint(x: vanishX, y: horizonY))
            p.addLine(to: CGPoint(x: bottomX, y: bottom))
            let alpha = 0.15 + 0.25 * (abs(t - 0.5) * 2)
            g.stroke(p, with: .color(Theme.gridPink.opacity(alpha)), lineWidth: 1)
        }

        // Scrolling horizontal lines (perspective — denser near horizon)
        let scroll = time.truncatingRemainder(dividingBy: 1.2) / 1.2   // 0..1, loops every 1.2s
        let bands = 8
        for i in 0..<bands {
            let t = (CGFloat(i) + CGFloat(scroll)) / CGFloat(bands)   // 0..1
            let eased = pow(t, 2.2)                                    // perspective compression
            let y = horizonY + eased * (bottom - horizonY)
            var p = Path()
            p.move(to: CGPoint(x: 0, y: y))
            p.addLine(to: CGPoint(x: size.width, y: y))
            let alpha = 0.45 * (1 - t) + 0.05
            g.stroke(p, with: .color(Theme.gridPink.opacity(alpha)), lineWidth: 1)
        }
    }

    private func drawScanlines(g: GraphicsContext, size: CGSize) {
        var step: CGFloat = 0
        while step < size.height {
            var p = Path()
            p.move(to: CGPoint(x: 0, y: step))
            p.addLine(to: CGPoint(x: size.width, y: step))
            g.stroke(p, with: .color(.black.opacity(0.12)), lineWidth: 1)
            step += 3
        }
    }
}
