/**
 * Join-code onboarding shared types.
 * Phase 1: local mock validation only (no real server).
 */
export var JoinState;
(function (JoinState) {
    JoinState["IDLE"] = "IDLE";
    JoinState["ENTERING"] = "ENTERING";
    JoinState["SUBMITTING"] = "SUBMITTING";
    JoinState["ACCEPTED"] = "ACCEPTED";
    JoinState["REJECTED"] = "REJECTED";
})(JoinState || (JoinState = {}));
/** The 30-char ambiguity-stripped character set (no 0/O, 1/I/L). */
export const CHAR_SET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789'.split('');
export const SLOT_COUNT = 6;
