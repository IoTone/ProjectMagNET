import SwiftUI

/// Space Invaders / Outrun crawdad on a 48x48 logical grid.
/// Two-frame marching body, bob, claw wiggle, antennae sway, state-driven eyes.
struct CrawdadView: View {
    let state: SessionState
    let sessionColor: Color
    let dimmed: Bool

    var body: some View {
        TimelineView(.animation(minimumInterval: 0.1, paused: false)) { context in
            Canvas { ctx, size in
                let scale = floor(min(size.width, size.height) / 48.0)
                guard scale >= 1 else { return }
                let offsetX = (size.width  - scale * 48) / 2
                let offsetY = (size.height - scale * 48) / 2
                let t = context.date.timeIntervalSince1970
                let bob = bobOffset(t: t)
                let env = DrawEnv(ctx: ctx, scale: scale, offsetX: offsetX, offsetY: offsetY + bob)
                drawShadow(env: env)
                drawAntennae(env: env, t: t)
                drawLegs(env: env, t: t)
                drawBody(env: env)
                drawClaws(env: env, t: t)
                drawAccents(env: env)
                drawEyes(env: env, time: context.date)
                if state == .working { drawMotionLines(env: env, t: t) }
            }
            .opacity(dimmed ? 0.45 : 1.0)
        }
    }

    // MARK: - Animation helpers

    /// Modulo-then-cast to dodge 32-bit Int overflow on arm64_32 watchOS.
    /// `t * speedHz` reaches ~10^10 for 2026-era timestamps, but Int32.max is ~2.1×10^9.
    private func phase(_ t: TimeInterval, speedHz: Double, mod: Int) -> Int {
        let modD = Double(mod)
        let bounded = (t * speedHz).truncatingRemainder(dividingBy: modD)
        let wrapped = bounded < 0 ? bounded + modD : bounded
        return Int(wrapped)
    }

    /// Vertical bob — pixelated (integer) so the crawdad marches Space-Invaders style.
    private func bobOffset(t: TimeInterval) -> CGFloat {
        switch state {
        case .working:   return pixelBob(t: t, speedHz: 4.0, amplitude: 2)
        case .needInput: return pixelBob(t: t, speedHz: 2.5, amplitude: 1)
        case .finished:  return pixelBob(t: t, speedHz: 1.0, amplitude: 1)
        case .error:     return pixelBob(t: t, speedHz: 3.0, amplitude: 1)
        case .idle:      return 0
        }
    }

    private func pixelBob(t: TimeInterval, speedHz: Double, amplitude: Int) -> CGFloat {
        let p = phase(t, speedHz: speedHz * 2, mod: 2)
        return CGFloat(p == 0 ? -amplitude : 0)
    }

    /// 2-frame leg shuffle returns 0 or 1.
    private func marchFrame(t: TimeInterval, speedHz: Double = 5.0) -> Int {
        phase(t, speedHz: speedHz, mod: 2)
    }

    // MARK: - Draw primitives

    private struct DrawEnv {
        let ctx: GraphicsContext
        let scale: CGFloat
        let offsetX: CGFloat
        let offsetY: CGFloat
    }

    private func pixel(_ env: DrawEnv, _ x: Int, _ y: Int, _ color: Color) {
        let rect = CGRect(
            x: env.offsetX + CGFloat(x) * env.scale,
            y: env.offsetY + CGFloat(y) * env.scale,
            width: env.scale,
            height: env.scale
        )
        env.ctx.fill(Path(rect), with: .color(color))
    }

    private func rect(_ env: DrawEnv, _ x: Int, _ y: Int, _ w: Int, _ h: Int, _ color: Color) {
        for i in 0..<w {
            for j in 0..<h {
                pixel(env, x + i, y + j, color)
            }
        }
    }

    // MARK: - Parts

    private func drawShadow(env: DrawEnv) {
        rect(env, 10, 44, 28, 1, Theme.gridPurple.opacity(0.9))
    }

    private func drawBody(env: DrawEnv) {
        // Core body: magenta with neon-pink outline
        rect(env, 12, 18, 24, 12, Theme.magenta)
        rect(env, 11, 19, 1, 10, Theme.hotPink)
        rect(env, 36, 19, 1, 10, Theme.hotPink)
        rect(env, 12, 17, 24, 1, Theme.hotPink)
        rect(env, 12, 30, 24, 1, Theme.hotPink)
        // Inner highlight stripe
        rect(env, 14, 20, 20, 1, Theme.hotPink.opacity(0.6))
    }

    private func drawClaws(env: DrawEnv, t: TimeInterval) {
        let frame = marchFrame(t: t, speedHz: 3.0)
        let open = (state == .needInput) || (state == .working && frame == 0)

        // Big right (fiddler) claw — opens/closes
        if open {
            rect(env, 36, 24,  4, 4, Theme.cyan)          // wrist
            rect(env, 40, 22,  5, 3, Theme.cyan)          // upper pincer
            rect(env, 40, 27,  5, 3, Theme.cyan)          // lower pincer
            rect(env, 45, 22,  1, 3, Theme.neonGreen)     // tip accent
            rect(env, 45, 27,  1, 3, Theme.neonGreen)
        } else {
            rect(env, 36, 24,  4, 4, Theme.cyan)
            rect(env, 40, 23,  6, 6, Theme.cyan)          // closed claw
            rect(env, 45, 25,  1, 2, Theme.neonGreen)
        }

        // Small left claw — opens/closes on opposite phase
        let leftOpen = !open
        if leftOpen {
            rect(env,  8, 22,  4, 3, Theme.cyan)
            rect(env,  5, 20,  3, 2, Theme.cyan)
            rect(env,  5, 25,  3, 2, Theme.cyan)
        } else {
            rect(env,  8, 22,  4, 4, Theme.cyan)
            rect(env,  5, 23,  3, 2, Theme.cyan)
        }
    }

    private func drawLegs(env: DrawEnv, t: TimeInterval) {
        let animate = (state == .working || state == .needInput)
        let frame = marchFrame(t: t, speedHz: 6.0)
        // 6 legs in two phases
        let baseY = 31
        for i in 0..<3 {
            let lx = 15 + i * 6
            let dy: Int
            if !animate {
                dy = 0
            } else if (i + frame) % 2 == 0 {
                dy = 0
            } else {
                dy = 2     // lifted leg
            }
            rect(env, lx,     baseY + dy, 1, 5, Theme.magenta)
            rect(env, lx + 3, baseY - dy, 1, 5, Theme.magenta)
        }
    }

    private func drawAntennae(env: DrawEnv, t: TimeInterval) {
        // Sway: alternate between two positions every 0.4s
        let sway = phase(t, speedHz: 2.5, mod: 2)
        let tilt = (state == .idle) ? 0 : (sway == 0 ? -1 : 1)
        rect(env, 18 + tilt, 10, 1, 8, Theme.hotPink)
        rect(env, 29 - tilt, 10, 1, 8, Theme.hotPink)
        // Antenna tips — session color
        pixel(env, 18 + tilt, 9, sessionColor)
        pixel(env, 29 - tilt, 9, sessionColor)
    }

    private func drawAccents(env: DrawEnv) {
        // Session color highlight on back ridge
        rect(env, 14, 18, 4, 1, sessionColor)
        rect(env, 30, 18, 4, 1, sessionColor)
    }

    private func drawMotionLines(env: DrawEnv, t: TimeInterval) {
        // Right-side speed lines when scurrying
        let frame = marchFrame(t: t, speedHz: 8.0)
        let color = Theme.cyan.opacity(0.6)
        if frame == 0 {
            rect(env, 46, 20, 2, 1, color)
            rect(env, 46, 26, 2, 1, color)
        } else {
            rect(env, 46, 22, 2, 1, color)
            rect(env, 46, 28, 2, 1, color)
        }
    }

    // MARK: - Eyes

    private func drawEyes(env: DrawEnv, time: Date) {
        let leftSocket  = (x: 16, y: 20, w: 5, h: 5)
        let rightSocket = (x: 27, y: 20, w: 5, h: 5)
        let t = time.timeIntervalSince1970
        switch state.eyeStyle {
        case .closed:
            closedEye(env: env, x: leftSocket.x,  y: leftSocket.y + 2)
            closedEye(env: env, x: rightSocket.x, y: rightSocket.y + 2)
        case .lookingAround:
            let p = phase(t, speedHz: 2.5, mod: 4)
            let dx: [Int] = [-1, 0, 1, 0]
            rect(env, leftSocket.x,  leftSocket.y,  4, 4, Theme.textBright)
            rect(env, rightSocket.x, rightSocket.y, 4, 4, Theme.textBright)
            rect(env, leftSocket.x  + 1 + dx[p], leftSocket.y  + 1, 2, 2, Theme.cyan)
            rect(env, rightSocket.x + 1 + dx[p], rightSocket.y + 1, 2, 2, Theme.cyan)
        case .squinting:
            let blink = phase(t, speedHz: 8.0, mod: 16) < 1
            squintEye(env: env, x: leftSocket.x,  y: leftSocket.y + 2, withGap: !blink)
            squintEye(env: env, x: rightSocket.x, y: rightSocket.y + 2, withGap: !blink)
        case .wideToSmall:
            let cycle = t.truncatingRemainder(dividingBy: 4) / 1.2
            let clamped = min(1.0, max(0.0, cycle))
            let sizeOpen: Int = clamped < 0.5 ? 4 : (clamped < 1.0 ? 3 : 2)
            let offset: Int = clamped < 0.5 ? 0 : 1
            rect(env, leftSocket.x  + offset, leftSocket.y  + offset, sizeOpen, sizeOpen, Theme.yellow)
            rect(env, rightSocket.x + offset, rightSocket.y + offset, sizeOpen, sizeOpen, Theme.yellow)
        case .asymmetric:
            let bounded = t.truncatingRemainder(dividingBy: 2.0)
            let pulse = abs(sin(bounded * .pi))
            let col = Theme.red.opacity(0.7 + 0.3 * pulse)
            rect(env, leftSocket.x, leftSocket.y, 4, 4, col)
            rect(env, leftSocket.x + 1, leftSocket.y + 1, 2, 2, .black)
            closedEye(env: env, x: rightSocket.x, y: rightSocket.y + 2)
        }
    }

    private func closedEye(env: DrawEnv, x: Int, y: Int) {
        rect(env, x, y, 4, 1, Theme.hotPink)
    }

    private func squintEye(env: DrawEnv, x: Int, y: Int, withGap: Bool) {
        if withGap {
            rect(env, x, y, 2, 1, Theme.neonGreen)
            rect(env, x + 3, y, 1, 1, Theme.neonGreen)
        } else {
            rect(env, x, y, 4, 1, Theme.neonGreen)
        }
    }
}
