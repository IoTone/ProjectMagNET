import SwiftUI

struct PromptPreview: View {
    let text: String

    @State private var crownOffset: Double = 0

    var body: some View {
        let content = text.isEmpty ? "—" : text
        ScrollView(.vertical, showsIndicators: false) {
            Text(content)
                .font(.system(size: 17, weight: .bold, design: .monospaced))
                .foregroundStyle(text.isEmpty ? Theme.textDim : Theme.textBright)
                .multilineTextAlignment(.leading)
                .lineLimit(nil)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
                .shadow(color: Theme.hotPink.opacity(0.35), radius: 2)
        }
        .focusable()
        .digitalCrownRotation($crownOffset, from: 0, through: 200, by: 1, sensitivity: .low, isContinuous: false, isHapticFeedbackEnabled: true)
    }
}
