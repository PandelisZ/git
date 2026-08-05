#!/bin/sh

# A custom image may supply VEX_GIT_HTTP_COMMAND.  The fallback is deliberately
# separate: it speaks stock smart HTTP over a classic bare repository volume
# and advertises vex_storage_active=false at /healthz.
set -eu

mkdir -p "${GIT_HTTP_ROOT:-/srv/git}" "${GIT_VEX_SCRATCH:-/var/git-scratch}"

if [ -n "${VEX_GIT_HTTP_COMMAND:-}" ]; then
	printf '%s\n' 'vex git-http: starting configured Vex-aware server command'
	exec /bin/sh -c "$VEX_GIT_HTTP_COMMAND"
fi

printf '%s\n' 'vex git-http: BOOTSTRAP CLASSIC-BARE MODE; Vex MySQL/MinIO storage is inactive'
exec python3 /usr/local/lib/vex-git-http/bootstrap_http.py
