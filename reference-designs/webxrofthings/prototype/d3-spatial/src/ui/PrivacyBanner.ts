/**
 * PrivacyBanner — first-render modal on dataspaces tagged `health-data`,
 * per specs/MagNET-Vitals-E4TH-proposal.md §11. Lists what the dataspace
 * exposes, where data goes, and asks for an explicit acknowledgement before
 * the user proceeds.
 *
 * Built with three-mesh-ui blocks + troika text, matching InspectorCard's
 * pattern. The "I understand" button registers with the Interact system
 * via the exported `acceptId` so existing mouse + XR-controller select
 * paths work without special-casing.
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from './palette';

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

  const title = new Text();
  title.text = 'Privacy notice';
  title.fontSize = 0.022;
  title.color = TEXT.primary;
  title.anchorX = 'center';
  title.anchorY = 'top';
  title.position.set(0, 0.135, 0.003);
  title.sync();
  card.add(title);

  const subtitle = new Text();
  subtitle.fontSize = 0.012;
  subtitle.color = TEXT.muted;
  subtitle.anchorX = 'center';
  subtitle.anchorY = 'top';
  subtitle.position.set(0, 0.108, 0.003);
  card.add(subtitle);

  const body = new Text();
  body.fontSize = 0.011;
  body.color = TEXT.body;
  body.anchorX = 'center';
  body.anchorY = 'top';
  body.position.set(0, 0.080, 0.003);
  body.maxWidth = 0.46;
  /* lineHeight is a real Troika prop; the .d.ts in this version omits it. */
  (body as unknown as { lineHeight: number }).lineHeight = 1.4;
  card.add(body);

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

  const acceptText = new Text();
  acceptText.text = 'I understand';
  acceptText.fontSize = 0.014;
  acceptText.color = TEXT.body;
  acceptText.anchorX = 'center';
  acceptText.anchorY = 'middle';
  acceptText.position.set(0, 0, 0.002);
  acceptText.sync();
  acceptButton.add(acceptText);

  function setFacts(facts: PrivacyFacts) {
    subtitle.text = `${facts.dataspaceName}  ·  scale: ${facts.scaleTag}`;
    subtitle.sync();

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
      lines.push('Data stays on your LAN — no cloud egress unless a remote');
      lines.push('service is added later.');
    }
    if (facts.tagsOfInterest.length > 0) {
      lines.push('');
      lines.push(`Flags: ${facts.tagsOfInterest.join('  ·  ')}`);
    }
    lines.push('');
    lines.push('Click "I understand" to enter the dataspace.');
    body.text = lines.join('\n');
    body.sync();
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
