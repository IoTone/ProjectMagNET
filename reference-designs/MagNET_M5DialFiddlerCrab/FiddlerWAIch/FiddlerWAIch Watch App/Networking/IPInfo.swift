import Foundation

/// Enumerates the local IP addresses assigned to this device.
/// Used purely for debugging — helps distinguish "watch is on Wi-Fi with a DHCP lease"
/// from "watch thinks it has Wi-Fi but has no IP."
enum IPInfo {

    struct Entry {
        let interface: String   // "en0", "en1", "pdp_ip0", "utun…", etc.
        let address: String     // IPv4 dotted or IPv6
        let isIPv4: Bool
    }

    static func currentAddresses() -> [Entry] {
        var results: [Entry] = []
        var head: UnsafeMutablePointer<ifaddrs>? = nil
        guard getifaddrs(&head) == 0, let first = head else { return results }
        defer { freeifaddrs(head) }

        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let p = cursor {
            defer { cursor = p.pointee.ifa_next }
            guard let addr = p.pointee.ifa_addr else { continue }
            let family = addr.pointee.sa_family
            guard family == sa_family_t(AF_INET) || family == sa_family_t(AF_INET6) else { continue }
            let name = String(cString: p.pointee.ifa_name)
            // Skip loopback; keep en*, pdp_ip* (cellular), and any other real interface.
            if name == "lo0" { continue }

            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            let saLen = socklen_t(family == sa_family_t(AF_INET) ? MemoryLayout<sockaddr_in>.size : MemoryLayout<sockaddr_in6>.size)
            let ok = getnameinfo(addr, saLen, &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
            guard ok == 0 else { continue }
            let ip = String(cString: host)
            // Skip IPv6 link-local (fe80::) noise.
            if ip.hasPrefix("fe80:") { continue }
            results.append(Entry(interface: name, address: ip, isIPv4: family == sa_family_t(AF_INET)))
        }
        return results
    }

    /// One-line summary for the debug overlay: `en0 192.168.1.42`
    static var summary: String {
        let all = currentAddresses()
        // Prefer IPv4 on en0 (Wi-Fi on watchOS) if present.
        if let wifiV4 = all.first(where: { $0.interface.hasPrefix("en") && $0.isIPv4 }) {
            return "\(wifiV4.interface) \(wifiV4.address)"
        }
        if let cellV4 = all.first(where: { $0.interface.hasPrefix("pdp_ip") && $0.isIPv4 }) {
            return "\(cellV4.interface) \(cellV4.address)"
        }
        if let first = all.first {
            return "\(first.interface) \(first.address)"
        }
        return "no addr"
    }
}
