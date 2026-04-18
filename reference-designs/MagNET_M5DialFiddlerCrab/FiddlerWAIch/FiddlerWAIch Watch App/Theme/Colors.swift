import SwiftUI

enum Theme {
    // Deep Outrun night sky
    static let bg         = Color(red: 0.039, green: 0.012, blue: 0.086)  // #0A0316
    static let bgMid      = Color(red: 0.094, green: 0.027, blue: 0.157)  // #180728 — horizon midtone

    // Neon core palette (boosted saturation)
    static let cyan       = Color(red: 0.0,   green: 1.0,   blue: 1.0)    // #00FFFF
    static let magenta    = Color(red: 1.0,   green: 0.0,   blue: 1.0)    // #FF00FF
    static let hotPink    = Color(red: 1.0,   green: 0.290, blue: 0.596)  // #FF4A98
    static let neonGreen  = Color(red: 0.224, green: 1.0,   blue: 0.078)  // #39FF14
    static let yellow     = Color(red: 1.0,   green: 0.898, blue: 0.0)    // #FFE500
    static let neonOrange = Color(red: 1.0,   green: 0.4,   blue: 0.0)    // #FF6600
    static let neonPurple = Color(red: 0.75,  green: 0.302, blue: 1.0)    // #BF4DFF
    static let red        = Color(red: 1.0,   green: 0.157, blue: 0.392)  // #FF2864

    // Text
    static let textBright = Color(red: 1.0,   green: 0.953, blue: 1.0)    // #FFF3FF — near-white pink tint
    static let textDim    = Color(red: 0.902, green: 0.608, blue: 0.859)  // #E69BDB — dim neon pink (readable on dark)
    static let textFaded  = Color(red: 0.596, green: 0.416, blue: 0.722)  // #986ABB — fallback for least-important text

    // Grid
    static let gridPink   = Color(red: 1.0,   green: 0.290, blue: 0.596)  // horizon lines
    static let gridPurple = Color(red: 0.40,  green: 0.12,  blue: 0.60)   // faded lines

    // Legacy alias (kept so older views compile until they're all swapped)
    static let dimGray    = gridPurple

    static let sessionPalette: [Color] = [cyan, magenta, hotPink, neonGreen, yellow]

    static func sessionColor(index: Int) -> Color {
        sessionPalette[index % sessionPalette.count]
    }
}
