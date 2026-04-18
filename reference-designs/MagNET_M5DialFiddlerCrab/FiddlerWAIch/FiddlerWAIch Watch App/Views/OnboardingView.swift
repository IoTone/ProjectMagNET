import SwiftUI

struct OnboardingView: View {
    @EnvironmentObject var settings: AppSettings
    @EnvironmentObject var mqtt: MQTTClient
    @State private var mac4Draft: String = ""

    var body: some View {
        ZStack {
            SynthwaveBackground()
            ScrollView {
                VStack(spacing: 12) {
                    Text("onboarding.title")
                        .font(.system(size: 20, weight: .black))
                        .foregroundStyle(Theme.cyan)
                        .shadow(color: Theme.cyan.opacity(0.85), radius: 4)
                    Text("onboarding.device_id_prompt")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundStyle(Theme.textDim)
                    TextField("", text: $mac4Draft)
                        .textContentType(nil)
                        .multilineTextAlignment(.center)
                        .font(.system(size: 26, weight: .black, design: .monospaced))
                        .foregroundStyle(Theme.textBright)
                        .frame(height: 46)
                        .background(Theme.bgMid)
                        .overlay(
                            RoundedRectangle(cornerRadius: 6).stroke(Theme.hotPink, lineWidth: 2)
                        )
                    Button {
                        let trimmed = AppSettings.sanitizeMac4(mac4Draft)
                        if AppSettings.isValidMac4(trimmed) {
                            settings.mac4 = trimmed
                            mqtt.resubscribe()
                        }
                    } label: {
                        Text("onboarding.save")
                            .font(.system(size: 16, weight: .black))
                            .frame(maxWidth: .infinity)
                    }
                    .tint(Theme.hotPink)
                    .disabled(!AppSettings.isValidMac4(AppSettings.sanitizeMac4(mac4Draft)))
                }
                .padding()
            }
        }
    }
}
