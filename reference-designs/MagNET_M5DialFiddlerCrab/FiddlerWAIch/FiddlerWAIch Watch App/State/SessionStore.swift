import Foundation
import Combine

@MainActor
final class SessionStore: ObservableObject {
    @Published private(set) var sessions: [Session] = []
    @Published var currentIndex: Int = 0

    private let maxSessions = 8
    private var nextColorIndex = 0
    private var previousStateBySession: [String: SessionState] = [:]
    private var feedback: FeedbackManager

    init(feedback: FeedbackManager) {
        self.feedback = feedback
    }

    func ingest(_ msg: MQTTMessage) {
        let now = Date()
        if let idx = sessions.firstIndex(where: { $0.id == msg.sessionId }) {
            let prev = sessions[idx].state
            sessions[idx].model = msg.model
            sessions[idx].state = msg.state
            sessions[idx].sessionPct = msg.sessionPct
            sessions[idx].weeklyPct = msg.weeklyPct
            sessions[idx].resetEpoch = msg.resetEpoch
            sessions[idx].clientHost = msg.clientHost
            if !msg.promptPreview.isEmpty {
                sessions[idx].promptPreview = msg.promptPreview
            }
            sessions[idx].lastUpdate = now
            applyTimerTransition(prev: prev, next: msg.state, session: &sessions[idx], now: now)
            emitTransitionFeedback(sessionId: msg.sessionId, prev: prev, next: msg.state, index: idx)
        } else {
            evictIfNeeded()
            let color = nextColorIndex
            nextColorIndex = (nextColorIndex + 1) % Theme.sessionPalette.count
            var new = Session(
                id: msg.sessionId,
                mac4: msg.mac4,
                model: msg.model,
                state: msg.state,
                sessionPct: msg.sessionPct,
                weeklyPct: msg.weeklyPct,
                resetEpoch: msg.resetEpoch,
                clientHost: msg.clientHost,
                promptPreview: msg.promptPreview,
                colorIndex: color,
                firstSeen: now,
                lastUpdate: now,
                workingStart: nil,
                workingAccumulated: 0
            )
            if msg.state == .working { new.workingStart = now }
            sessions.append(new)
            emitTransitionFeedback(sessionId: msg.sessionId, prev: .idle, next: msg.state, index: sessions.count - 1)
        }
        previousStateBySession[msg.sessionId] = msg.state
        clampIndex()
    }

    func pruneExpired(now: Date = Date()) {
        let before = sessions.count
        sessions.removeAll { $0.isExpired }
        if sessions.count != before {
            clampIndex()
        }
    }

    func next() {
        guard !sessions.isEmpty else { return }
        currentIndex = (currentIndex + 1) % sessions.count
        feedback.swipeClick()
    }

    func previous() {
        guard !sessions.isEmpty else { return }
        currentIndex = (currentIndex - 1 + sessions.count) % sessions.count
        feedback.swipeClick()
    }

    // MARK: - Internals

    private func applyTimerTransition(prev: SessionState, next: SessionState, session: inout Session, now: Date) {
        let wasWorking = prev == .working
        let isWorking = next == .working
        switch (wasWorking, isWorking) {
        case (false, true):
            session.workingStart = now
        case (true, false):
            if let start = session.workingStart {
                session.workingAccumulated += now.timeIntervalSince(start)
            }
            session.workingStart = nil
        default:
            break
        }
    }

    private func emitTransitionFeedback(sessionId: String, prev: SessionState, next: SessionState, index: Int) {
        guard prev != next else { return }
        let isVisible = (index == currentIndex)
        feedback.stateTransition(to: next, visible: isVisible, sessionId: sessionId)
    }

    private func evictIfNeeded() {
        guard sessions.count >= maxSessions else { return }
        if let idx = sessions.firstIndex(where: { $0.state != .working }) {
            sessions.remove(at: idx)
        } else {
            let oldestIdx = sessions.indices.min(by: { sessions[$0].lastUpdate < sessions[$1].lastUpdate }) ?? 0
            sessions.remove(at: oldestIdx)
        }
        clampIndex()
    }

    private func clampIndex() {
        if sessions.isEmpty {
            currentIndex = 0
        } else if currentIndex >= sessions.count {
            currentIndex = sessions.count - 1
        } else if currentIndex < 0 {
            currentIndex = 0
        }
    }
}
