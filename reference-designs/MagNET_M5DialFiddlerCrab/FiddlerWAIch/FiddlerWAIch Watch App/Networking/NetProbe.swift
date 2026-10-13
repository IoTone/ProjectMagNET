import Foundation
import os

/// Connectivity probes, shared by the 設定 screen's 接続テスト button and the automatic
/// run at launch.
///
/// The point is to separate "this device has no network" from "this device has network but
/// denies raw socket flows to third-party apps." Those produce identical user-visible
/// symptoms and identical `localizedDescription` text, and differ only in which probe
/// survives. `dataTask` traffic can be carried by the watch's proxied companion path,
/// while `webSocketTask` and `NWConnection` need a satisfied path of their own — so
/// `https` succeeding while `ws`/`wss` fail is the signature of the latter.
enum NetProbe {

    /// Runs every probe concurrently and returns one line per probe, e.g.
    /// `https:204  get:400  ws:-1009  wss:-1009`.
    static func runAll() async -> [String] {
        async let https  = http(url: "https://www.google.com/generate_204", label: "https")
        async let get    = http(url: "http://broker.hivemq.com:8000/mqtt",  label: "get")
        async let ws     = webSocket(url: "ws://broker.hivemq.com:8000/mqtt",   label: "ws")
        async let wss    = webSocket(url: "wss://broker.hivemq.com:8884/mqtt", label: "wss")
        return await [https, get, ws, wss]
    }

    /// One-shot run whose results land in the same log stream as everything else, so the
    /// answer shows up in the Xcode console without anyone tapping through to 設定.
    static func runAtLaunch() {
        Task {
            let results = await runAll()
            await MainActor.run {
                MQTTLog.shared.append("[PROBE] " + results.joined(separator: "  "))
            }
        }
    }

    static func http(url: String, label: String) async -> String {
        guard let u = URL(string: url) else { return "\(label):bad-url" }
        var req = URLRequest(url: u)
        req.timeoutInterval = 8
        do {
            let (_, resp) = try await URLSession.shared.data(for: req)
            return "\(label):\((resp as? HTTPURLResponse)?.statusCode ?? -1)"
        } catch {
            return "\(label):\((error as NSError).code)"
        }
    }

    static func webSocket(url: String, label: String) async -> String {
        guard let u = URL(string: url) else { return "\(label):bad-url" }
        return await withCheckedContinuation { (cont: CheckedContinuation<String, Never>) in
            let task = URLSession.shared.webSocketTask(with: u, protocols: ["mqtt"])
            // Only the first resume of the continuation counts; the timeout below races
            // against the receive callback.
            let done = OSAllocatedUnfairLock(initialState: false)
            func finish(_ value: String) {
                let first = done.withLock { already -> Bool in
                    if already { return false }
                    already = true
                    return true
                }
                if first { cont.resume(returning: value) }
            }
            task.resume()
            // Ping, don't receive. An MQTT broker sends nothing until it gets a CONNECT, so
            // waiting on `receive` times out on a perfectly healthy socket — success and
            // failure were indistinguishable. A WebSocket ping round-trips at the protocol
            // level: the pong handler fires with nil only if the upgrade actually completed.
            task.sendPing { error in
                if let error {
                    finish("\(label):\((error as NSError).code)")
                } else {
                    task.cancel(with: .normalClosure, reason: nil)
                    finish("\(label):OK")
                }
            }
            DispatchQueue.global().asyncAfter(deadline: .now() + 10) {
                task.cancel(with: .abnormalClosure, reason: nil)
                finish("\(label):timeout")
            }
        }
    }
}
