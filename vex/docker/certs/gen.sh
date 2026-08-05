#!/usr/bin/env bash

# Generate a small local development CA and a localhost certificate.  The
# script is intentionally idempotent so `scripts/vex/up.sh` can call it every
# time without rotating an already trusted CA.
set -euo pipefail

cert_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ca_cert="$cert_dir/rootCA.pem"
ca_key="$cert_dir/rootCA.key"
cert="$cert_dir/localhost.pem"
key="$cert_dir/localhost.key"

if [ -s "$ca_cert" ] && [ -s "$ca_key" ] && [ -s "$cert" ] && [ -s "$key" ]; then
	exit 0
fi

if ! command -v openssl >/dev/null 2>&1; then
	printf '%s\n' 'vex certs: openssl is required to create the local TLS certificate' >&2
	exit 127
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/vex-certs.XXXXXX")
cleanup() {
	rm -rf -- "$work"
}
trap cleanup EXIT HUP INT TERM

umask 077
openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 \
	-subj '/CN=Vex local development CA' \
	-keyout "$work/rootCA.key" -out "$work/rootCA.pem" >/dev/null 2>&1
openssl req -newkey rsa:2048 -sha256 -nodes \
	-subj '/CN=localhost' \
	-keyout "$work/localhost.key" -out "$work/localhost.csr" >/dev/null 2>&1
printf '%s\n' \
	'[v3_req]' \
	'subjectAltName = DNS:localhost,IP:127.0.0.1' \
	'keyUsage = digitalSignature,keyEncipherment' \
	'extendedKeyUsage = serverAuth' >"$work/localhost.ext"
openssl x509 -req -sha256 -days 825 \
	-in "$work/localhost.csr" \
	-CA "$work/rootCA.pem" -CAkey "$work/rootCA.key" -CAcreateserial \
	-extfile "$work/localhost.ext" -extensions v3_req \
	-out "$work/localhost.pem" >/dev/null 2>&1

mv "$work/rootCA.pem" "$ca_cert"
mv "$work/rootCA.key" "$ca_key"
mv "$work/localhost.pem" "$cert"
mv "$work/localhost.key" "$key"
chmod 600 "$ca_key" "$key"
chmod 644 "$ca_cert" "$cert"
printf 'vex certs: generated local CA at %s\n' "$ca_cert"
