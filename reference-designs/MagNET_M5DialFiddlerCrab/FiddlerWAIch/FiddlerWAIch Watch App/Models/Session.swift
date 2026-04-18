import Foundation

struct Session: Identifiable, Equatable {
    let id: String
    var mac4: String
    var model: String
    var state: SessionState
    var sessionPct: Int
    var weeklyPct: Int
    var resetEpoch: UInt64
    var clientHost: String
    var promptPreview: String
    var colorIndex: Int
    let firstSeen: Date
    var lastUpdate: Date
    var workingStart: Date?
    var workingAccumulated: TimeInterval

    var workingElapsed: TimeInterval {
        if let start = workingStart {
            return workingAccumulated + Date().timeIntervalSince(start)
        }
        return workingAccumulated
    }

    var isStale: Bool {
        Date().timeIntervalSince(lastUpdate) > 5 * 60
    }

    var isExpired: Bool {
        Date().timeIntervalSince(lastUpdate) > 30 * 60
    }
}
