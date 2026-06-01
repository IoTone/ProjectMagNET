import Foundation
import Network

/// Watches the device's overall network path via NWPathMonitor.
/// Useful for distinguishing "broker unreachable" from "no network at all."
@MainActor
final class NetworkMonitor: ObservableObject {
    @Published private(set) var pathStatus: String = "—"
    @Published private(set) var interface: String = "—"
    @Published private(set) var isReachable: Bool = false

    private let monitor = NWPathMonitor()
    private let queue = DispatchQueue(label: "fiddlerwaich.netmonitor")

    init() {
        monitor.pathUpdateHandler = { [weak self] path in
            Task { @MainActor [weak self] in self?.apply(path: path) }
        }
        monitor.start(queue: queue)
    }

    private func apply(path: NWPath) {
        switch path.status {
        case .satisfied:        pathStatus = "OK"
        case .unsatisfied:      pathStatus = "NO NET"
        case .requiresConnection: pathStatus = "WAIT"
        @unknown default:       pathStatus = "?"
        }
        isReachable = path.status == .satisfied

        if path.usesInterfaceType(.wifi) {
            interface = "wifi"
        } else if path.usesInterfaceType(.cellular) {
            interface = "cell"
        } else if path.usesInterfaceType(.wiredEthernet) {
            interface = "wired"
        } else if path.usesInterfaceType(.loopback) {
            interface = "loop"
        } else if path.usesInterfaceType(.other) {
            interface = "other"
        } else {
            interface = "—"
        }
        print("[NET] path=\(pathStatus) iface=\(interface) gateways=\(path.gateways.count)")
    }
}
