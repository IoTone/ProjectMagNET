#!/bin/sh
# genkeys.sh — generate the Hanasu release keys with openssl only (no Python).
# Run on the SIGNING machine. Hand over only the *.pub.pem files.
#
#   sh genkeys.sh [--ota-p256] [outdir]          (default outdir: ./release-keys)
#
#   ota_release.pem       + .pub.pem   OTA package key, current   (Ed25519, or P-256)
#   ota_release_next.pem  + .pub.pem   OTA package key, next      (rotation spare)
#   mcuboot_release.pem   + .pub.pem   MG24 MCUboot image key     (ECDSA P-256)
#
# Ed25519 needs OpenSSL >= 1.1.1 (any current Linux; on macOS the system
# openssl is LibreSSL without Ed25519: `brew install openssl@3` and run with
# OPENSSL=$(brew --prefix openssl@3)/bin/openssl). --ota-p256 makes the OTA keys
# P-256 instead (sig_alg 1, fully supported; Ed25519 is preferred), which works
# with the stock macOS openssl.
#
# Private keys: PKCS#8 PEM, mode 600, never overwritten. The printed key_id is
# SHA-256(raw public key)[0:8], exactly what tools/mnpkg.py and the nodes use.
set -eu
umask 077

OTA_ALG=ed25519
if [ "${1:-}" = "--ota-p256" ]; then OTA_ALG=p256; shift; fi
OUT=${1:-release-keys}
OPENSSL=${OPENSSL:-openssl}

command -v "$OPENSSL" >/dev/null 2>&1 || { echo "genkeys: $OPENSSL not found" >&2; exit 1; }
if [ "$OTA_ALG" = ed25519 ] &&
   ! "$OPENSSL" genpkey -algorithm ed25519 >/dev/null 2>&1; then
    echo "genkeys: '$OPENSSL' ($("$OPENSSL" version)) has no Ed25519." >&2
    echo "  use OpenSSL 3:  OPENSSL=\$(brew --prefix openssl@3)/bin/openssl sh $0 $OUT" >&2
    echo "  or P-256 OTA keys with this openssl:  sh $0 --ota-p256 $OUT" >&2
    exit 1
fi

mkdir -p "$OUT"
for f in ota_release ota_release_next mcuboot_release; do
    [ -e "$OUT/$f.pem" ] && { echo "genkeys: $OUT/$f.pem exists — refusing to overwrite a key" >&2; exit 1; }
done

gen() {   # gen <name> <ed25519|p256>
    k="$OUT/$1.pem"
    if [ "$2" = ed25519 ]; then
        "$OPENSSL" genpkey -algorithm ed25519 -out "$k"
    else  # ecparam + pkcs8: works on OpenSSL and LibreSSL alike
        "$OPENSSL" ecparam -name prime256v1 -genkey -noout |
            "$OPENSSL" pkcs8 -topk8 -nocrypt -out "$k"
    fi
    chmod 600 "$k"
    "$OPENSSL" pkey -in "$k" -pubout -out "$OUT/$1.pub.pem"
    chmod 644 "$OUT/$1.pub.pem"
}

key_id() {  # key_id <pub.pem> <ed25519|p256>: SHA-256 of the raw key, first 8 bytes
    n=65; [ "$2" = ed25519 ] && n=32          # raw key = the tail of the SPKI DER
    "$OPENSSL" pkey -pubin -in "$1" -outform DER | tail -c "$n" |
        "$OPENSSL" dgst -sha256 -binary | od -An -tx1 | tr -d ' \n' | cut -c1-16
}

gen ota_release      "$OTA_ALG"
gen ota_release_next "$OTA_ALG"
gen mcuboot_release  p256

echo "Generated in $OUT/ with $("$OPENSSL" version):"
printf '  %-22s %-8s key_id %s\n' ota_release.pem      "$OTA_ALG" "$(key_id "$OUT/ota_release.pub.pem" "$OTA_ALG")"
printf '  %-22s %-8s key_id %s\n' ota_release_next.pem "$OTA_ALG" "$(key_id "$OUT/ota_release_next.pub.pem" "$OTA_ALG")"
printf '  %-22s %-8s (MCUboot keys have no key_id)\n' mcuboot_release.pem p256
echo
echo "Back up the three private .pem files offline. Hand over ONLY:"
ls -1 "$OUT"/*.pub.pem | sed 's/^/  /'
