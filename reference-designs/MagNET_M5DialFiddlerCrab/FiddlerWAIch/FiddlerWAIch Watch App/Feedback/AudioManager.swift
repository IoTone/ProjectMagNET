import Foundation
import AVFoundation

final class AudioManager {
    private var players: [String: AVAudioPlayer] = [:]
    private var didSetup = false

    init() {
        // Defer audio session + CAF preload to first playback — avoids blocking app launch.
    }

    private func ensureSetup() {
        guard !didSetup else { return }
        didSetup = true
        configureSession()
        preload()
    }

    private func configureSession() {
        try? AVAudioSession.sharedInstance().setCategory(.ambient, options: [.mixWithOthers])
        try? AVAudioSession.sharedInstance().setActive(true)
    }

    private func preload() {
        let names = [
            "chirp_working", "chirp_finished", "chirp_needinput", "chirp_error",
            "click_swipe", "beep_connected", "beep_disconnected"
        ]
        for name in names {
            guard let url = Bundle.main.url(forResource: name, withExtension: "caf") else { continue }
            if let player = try? AVAudioPlayer(contentsOf: url) {
                player.prepareToPlay()
                players[name] = player
            }
        }
    }

    private func play(_ name: String) {
        ensureSetup()
        guard let player = players[name] else { return }
        if player.isPlaying { player.currentTime = 0 }
        player.play()
    }

    func stateTransition(to state: SessionState) {
        switch state {
        case .working:   play("chirp_working")
        case .finished:  play("chirp_finished")
        case .needInput: play("chirp_needinput")
        case .error:     play("chirp_error")
        case .idle:      break
        }
    }

    func swipeClick()   { play("click_swipe") }
    func connected()    { play("beep_connected") }
    func disconnected() { play("beep_disconnected") }
}
