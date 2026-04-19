/**
 * Join-code onboarding shared types.
 * Phase 1: local mock validation only (no real server).
 */

export enum JoinState {
  IDLE = 'IDLE',
  ENTERING = 'ENTERING',
  SUBMITTING = 'SUBMITTING',
  ACCEPTED = 'ACCEPTED',
  REJECTED = 'REJECTED',
}

/** The 30-char ambiguity-stripped character set (no 0/O, 1/I/L). */
export const CHAR_SET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789'.split('');

export const SLOT_COUNT = 6;

export interface JoinPanelEvents {
  onAccepted?: (code: string) => void;
  onRejected?: (code: string, reason: string) => void;
  onCancel?: () => void;
}
