/**
 * PrivacyBanner — first-render modal on dataspaces tagged `health-data`,
 * per specs/MagNET-Vitals-E4TH-proposal.md §11. Lists what the dataspace
 * exposes, where data goes, and asks for an explicit acknowledgement before
 * the user proceeds.
 *
 * Built entirely with three-mesh-ui blocks + MSDF text (no troika) so every
 * glyph stays crisp on optical passthrough. Each text region lives inside
 * its own invisible Block wrapper that owns the layout slot; this preserves
 * the prior absolute layout (positions tuned to the card frame) while
 * giving every Text instance the parent-Block-with-font context that
 * ThreeMeshUI.Text needs.
 *
 * The "I understand" button registers with the Interact system via the
 * exported `acceptId` so existing mouse + XR-controller select paths work
 * without special-casing.
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { TEXT } from './palette';
import { FONT_BLOCK_OPTS, fontColor, sanitizeText, makeSlot } from './textStyles';

export interface PrivacyFacts {
  dataspaceName: string;
  scaleTag: string;
  deviceCount: number;
  serviceCount: number;
  /** Tags worth highlighting (filtered down to the privacy-relevant set). */
  tagsOfInterest: string[];
  hasHealthData: boolean;
  isLanOnly: boolean;
}

export interface PrivacyBanner {
  group: THREE.Group;
  /** three-mesh-ui Block. Typed `any` because three-mesh-ui's bundled types
   *  don't expose `set()` even though it's a documented API — same dodge
   *  used in InspectorCard and Toolbar throughout this codebase. */
  acceptButton: any;
  show(): void;
  hide(): void;
  visible(): boolean;
  setFacts(facts: PrivacyFacts): void;
  acceptId: string;
}

const ACCEPT_ID = 'privacy-banner:accept';

export function createPrivacyBanner(): PrivacyBanner {
  const group = new THREE.Group();
  group.name = 'privacy-banner';
  group.visible = false;

  const card = new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width: 0.50,
    height: 0.32,
    padding: 0.018,
    borderRadius: 0.012,
    backgroundColor: new THREE.Color(0x2a2520),
    backgroundOpacity: 0.96,
    borderWidth: 0.0015,
    borderColor: new THREE.Color(0xff7a8a), // warm coral — privacy/health emphasis
    borderOpacity: 0.95,
  });
  group.add(card);

  // ─── Title slot ─────────────────────────────────────────────────────
  const titleSlot = makeSlot(0.46, 0.030);
  titleSlot.position.set(0, 0.120, 0.003);
  card.add(titleSlot);
  titleSlot.add(new ThreeMeshUI.Text({
    content: 'Privacy notice',
    fontSize: 0.022,
    fontColor: fontColor(TEXT.primary),
  }));

  // ─── Subtitle slot ─────────────────────────────────────────────────
  const subtitleSlot = makeSlot(0.46, 0.020);
  subtitleSlot.position.set(0, 0.090, 0.003);
  card.add(subtitleSlot);
  // `any` cast: ThreeMeshUI.Text's types don't expose `set()` even though
  // it's a documented runtime API — same workaround the Block bindings use.
  const subtitleText: any = new ThreeMeshUI.Text({
    content: '',
    fontSize: 0.012,
    fontColor: fontColor(TEXT.muted),
  });
  subtitleSlot.add(subtitleText);

  // ─── Body slot (multi-line, wrapping) ─────────────────────────────
  // Block width controls wrap; height is generous because we don't know
  // how many lines the facts produce until setFacts() runs.
  const bodySlot = new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width: 0.46, height: 0.16,
    backgroundOpacity: 0,
    borderOpacity: 0,
    padding: 0,
    justifyContent: 'start',
    alignItems: 'center',
    textAlign: 'center',
  });
  bodySlot.position.set(0, -0.005, 0.003);
  card.add(bodySlot);
  const bodyText: any = new ThreeMeshUI.Text({
    content: '',
    fontSize: 0.011,
    fontColor: fontColor(TEXT.body),
  });
  bodySlot.add(bodyText);

  // ─── Accept button ─────────────────────────────────────────────────
  const acceptButton = new ThreeMeshUI.Block({
    width: 0.18,
    height: 0.038,
    padding: 0.006,
    borderRadius: 0.008,
    backgroundColor: new THREE.Color(0x4a3530),
    backgroundOpacity: 0.92,
    borderWidth: 0.001,
    borderColor: new THREE.Color(0xff7a8a),
    borderOpacity: 0.95,
    justifyContent: 'center',
    alignItems: 'center',
  });
  acceptButton.position.set(0, -0.118, 0.005);
  acceptButton.name = ACCEPT_ID;
  card.add(acceptButton);
  acceptButton.add(new ThreeMeshUI.Text({
    content: 'I understand',
    fontSize: 0.014,
    fontColor: fontColor(TEXT.body),
  }));

  function setFacts(facts: PrivacyFacts) {
    // sanitizeText scrubs middle dots / em dashes / other glyphs the bundled
    // Roboto-msdf doesn't carry. Subtitle and body both pull values from the
    // manifest (and manifest authors aren't obligated to think about font
    // coverage), so wrapping every Text mutation here is the safe play.
    subtitleText.set({
      content: sanitizeText(`${facts.dataspaceName}  -  scale: ${facts.scaleTag}`),
    });

    const lines: string[] = [];
    lines.push(
      `This dataspace exposes ${facts.deviceCount} device${facts.deviceCount === 1 ? '' : 's'}` +
      ` and ${facts.serviceCount} service${facts.serviceCount === 1 ? '' : 's'}.`,
    );
    if (facts.hasHealthData) {
      lines.push('');
      lines.push('Health data on display includes heart rate, breathing rate,');
      lines.push('presence and multi-target position. Treat as personal.');
    }
    if (facts.isLanOnly) {
      lines.push('');
      lines.push('Data stays on your LAN -- no cloud egress unless a remote');
      lines.push('service is added later.');
    }
    if (facts.tagsOfInterest.length > 0) {
      lines.push('');
      lines.push(`Flags: ${facts.tagsOfInterest.join(' - ')}`);
    }
    lines.push('');
    lines.push('Click "I understand" to enter the dataspace.');
    bodyText.set({ content: sanitizeText(lines.join('\n')) });
  }

  return {
    group,
    acceptButton,
    show: () => { group.visible = true; },
    hide: () => { group.visible = false; },
    visible: () => group.visible,
    setFacts,
    acceptId: ACCEPT_ID,
  };
}

/** Compute the privacy facts from a loaded dataspace manifest. */
export function privacyFactsFromManifest(manifest: {
  name: string;
  scaleTag: string;
  udm_devices?: Array<{ udm_tags?: string[] }>;
  usm_services?: unknown[];
}): PrivacyFacts {
  const interestingTags = new Set<string>();
  let hasHealthData = false;
  let isLanOnly = false;
  for (const dev of manifest.udm_devices ?? []) {
    for (const t of dev.udm_tags ?? []) {
      if (t === 'health-data')   { hasHealthData = true; interestingTags.add(t); }
      else if (t === 'lan-only') { isLanOnly = true;     interestingTags.add(t); }
      else if (t === 'sleep-only' || t === 'no-tls' || t === 'non-contact' ||
               t === 'biometric'  || t === 'personal') {
        interestingTags.add(t);
      }
    }
  }
  return {
    dataspaceName: manifest.name,
    scaleTag: manifest.scaleTag,
    deviceCount: (manifest.udm_devices ?? []).length,
    serviceCount: (manifest.usm_services ?? []).length,
    tagsOfInterest: [...interestingTags],
    hasHealthData,
    isLanOnly,
  };
}
