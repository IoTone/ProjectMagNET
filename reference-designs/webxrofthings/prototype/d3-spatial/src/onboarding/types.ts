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

/**
 * Full 36-char alphanumeric set used by the slot wheel and keyboard input.
 *
 * Earlier this was ambiguity-stripped to A-Z minus O/I/L plus digits 2-9
 * (no 0/1) for cleaner handoff of rotating codes. That made the fixed
 * UC codes (DEMO01-04) literally untypable in the wheel because they
 * contain O, 0, and 1. Reverted to the full set so the demo codes work;
 * handoff readability of rotating codes takes a minor hit.
 */
export const CHAR_SET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'.split('');

export const SLOT_COUNT = 6;

export interface JoinPanelEvents {
  onAccepted?: (code: string, token?: string, manifestUrl?: string, dataspace?: string) => void;
  onRejected?: (code: string, reason: string) => void;
  onCancel?: () => void;
}
