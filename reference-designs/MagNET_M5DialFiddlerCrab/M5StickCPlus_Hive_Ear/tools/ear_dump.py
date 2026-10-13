#!/usr/bin/env python3
"""Turn an Ear `mic-dump` (base64 IMA-ADPCM between markers) into a WAV.

    ear_dump.py console.log out.wav

The log can contain anything else; only the last BEGIN/END block is used.
Decoder is the same IMA/DVI ADPCM (high nibble first) the ATVV receiver uses,
so a clean WAV here also validates the on-device encoder.
"""
import base64, re, sys, wave, struct

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_decode(data, pred=0, idx=0):
    out = []
    for b in data:
        for nib in (b >> 4, b & 0x0F):
            step = STEP[idx]
            diff = step >> 3
            if nib & 1: diff += step >> 2
            if nib & 2: diff += step >> 1
            if nib & 4: diff += step
            pred = max(-32768, min(32767, pred - diff if nib & 8 else pred + diff))
            idx = max(0, min(88, idx + IDX[nib & 7]))
            out.append(pred)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    text = open(sys.argv[1], errors="replace").read()
    blocks = re.findall(r"-----BEGIN EAR ADPCM rate=(\d+) pred=(-?\d+) idx=(\d+) bytes=(\d+)-----"
                        r"(.*?)-----END EAR ADPCM-----", text, re.S)
    if not blocks:
        sys.exit("no EAR ADPCM block found")
    rate, pred, idx, nbytes, body = blocks[-1]
    raw = base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]", "", body))
    if len(raw) != int(nbytes):
        print(f"warning: got {len(raw)} bytes, header says {nbytes}", file=sys.stderr)
    pcm = ima_decode(raw, int(pred), int(idx))
    with wave.open(sys.argv[2], "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(rate))
        w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
    peak = max(abs(s) for s in pcm) if pcm else 0
    rms = (sum(s * s for s in pcm) / max(1, len(pcm))) ** 0.5
    print(f"{sys.argv[2]}: {len(pcm)} samples @ {rate} Hz = {len(pcm)/int(rate):.2f} s, "
          f"peak {peak}, rms {rms:.0f}")


if __name__ == "__main__":
    main()
