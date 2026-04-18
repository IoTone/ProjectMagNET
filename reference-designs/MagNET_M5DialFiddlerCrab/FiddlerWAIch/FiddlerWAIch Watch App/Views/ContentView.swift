import SwiftUI

struct ContentView: View {
    @EnvironmentObject var settings: AppSettings

    var body: some View {
        ZStack(alignment: .topLeading) {
            if settings.mac4.isEmpty {
                OnboardingView()
            } else {
                SessionPagerView()
            }
            // Always-visible build stamp so you can verify the reinstall actually took.
            Text("b\(AppSettings.buildTimestamp)")
                .font(.system(size: 9, weight: .black, design: .monospaced))
                .foregroundStyle(Theme.neonOrange)
                .padding(.horizontal, 4)
                .padding(.vertical, 2)
                .background(Color.black.opacity(0.7))
                .cornerRadius(3)
                .padding(.top, 2)
                .padding(.leading, 2)
        }
    }
}
