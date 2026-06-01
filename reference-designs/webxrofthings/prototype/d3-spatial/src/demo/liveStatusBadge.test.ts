import { describe, it, expect } from 'vitest';
import { formatOfflineLabel } from './liveStatusBadge';

describe('formatOfflineLabel', () => {
  it('returns plain "OFFLINE" when no last-success timestamp is known', () => {
    expect(formatOfflineLabel(null)).toBe('OFFLINE');
  });

  it('formats sub-minute durations as seconds', () => {
    expect(formatOfflineLabel(0)).toBe('OFFLINE · 0s');
    expect(formatOfflineLabel(900)).toBe('OFFLINE · 0s');         // < 1s rounds down
    expect(formatOfflineLabel(1000)).toBe('OFFLINE · 1s');
    expect(formatOfflineLabel(42_000)).toBe('OFFLINE · 42s');
    expect(formatOfflineLabel(59_999)).toBe('OFFLINE · 59s');
  });

  it('formats whole minutes without a trailing seconds component', () => {
    expect(formatOfflineLabel(60_000)).toBe('OFFLINE · 1m');
    expect(formatOfflineLabel(120_000)).toBe('OFFLINE · 2m');
    expect(formatOfflineLabel(600_000)).toBe('OFFLINE · 10m');
  });

  it('formats minute+seconds when there is a remainder', () => {
    expect(formatOfflineLabel(154_000)).toBe('OFFLINE · 2m 34s');
    expect(formatOfflineLabel(3_661_000)).toBe('OFFLINE · 61m 1s');   // not bothering with hours
  });
});
