#!/usr/bin/env bash
#
# Joins the Codespace to the tailnet, so `make flash` and `make console`
# reach the flatsat (docs/flatsat.md, DS-04).
#
# The Tailscale feature tries this itself, but only when TS_AUTH_KEY is set
# as the container starts, and Codespaces adds its secrets later than that.
# So this runs as the postStartCommand, after every start, once the secret
# is there. Without the secret, or already on the tailnet, it does nothing.
#
# It never fails the Codespace's start: a Codespace off the tailnet still
# builds and tests; it just can't reach the flatsat.

if [ -z "${TS_AUTH_KEY:-}" ]; then
	echo "==> tailscale: no TS_AUTH_KEY secret, not joining the tailnet"
	exit 0
fi

if tailscale status --peers=false --json 2>/dev/null |
		grep -q '"BackendState": "Running"'; then
	echo "==> tailscale: already on the tailnet"
	exit 0
fi

# The same flags the feature uses. The key carries tag:codespace.
echo "==> tailscale: joining the tailnet"
if ! sudo tailscale up --accept-routes --authkey="$TS_AUTH_KEY" \
		--hostname="${CODESPACE_NAME:-codespace}"; then
	echo "WARNING: tailscale up failed; see docs/flatsat.md," \
		"\"When something's wrong\"" >&2
fi
exit 0
