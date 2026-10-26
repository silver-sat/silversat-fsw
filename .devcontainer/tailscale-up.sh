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

# Self.Online, not BackendState. "Running" only means the daemon is up:
# the Tailscale feature starts tailscaled at container start, before the
# Codespace secrets exist, so it sits there logged out with an AuthURL,
# no peers, and BackendState "Running". Testing BackendState here made
# this script report success on a Codespace that had never joined, and
# skip the one thing it exists to do. --peers=false leaves only Self, so
# there is no other node's Online to match by accident.
if tailscale status --peers=false --json 2>/dev/null |
		grep -q '"Online": true'; then
	echo "==> tailscale: already on the tailnet"
	exit 0
fi

# The same flags the feature uses. The key carries tag:codespace.
#
# The key goes through a file, never on the command line: anything on a
# command line shows in `ps` to everyone in the Codespace. printf is built
# into bash, so writing the file doesn't put the key on one either.
#
# --timeout stops a join that can't finish (say, a key that isn't
# pre-approved, waiting for an admin) from holding up the Codespace's start.
# tailscaled keeps waiting in the background, and joins if it's approved.
echo "==> tailscale: joining the tailnet"
key_file=$(mktemp)
chmod 600 "$key_file"
printf '%s' "$TS_AUTH_KEY" > "$key_file"
if ! sudo tailscale up --accept-routes --auth-key="file:$key_file" \
		--hostname="${CODESPACE_NAME:-codespace}" --timeout=60s; then
	echo "WARNING: couldn't join the tailnet; \`tailscale status\` says why." \
		"See docs/flatsat.md, \"When something's wrong\"" >&2
fi
rm -f "$key_file"
exit 0
