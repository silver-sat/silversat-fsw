# SPDX-License-Identifier: Apache-2.0
"""Tests for .devcontainer/tailscale-up.sh. Run with `make test-python`.

Stand-ins for sudo and tailscale, first on PATH, record every command line
and what the key file held, so the tests can check the key never appears
on a command line (where `ps` would show it) and the file is gone after.
"""

import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / ".devcontainer" / "tailscale-up.sh"
KEY = "tskey-auth-TESTONLY-0123456789abcdef"

TAILSCALE = r"""#!/usr/bin/env bash
echo "$*" >> "$LOG"
if [ "$1" = status ]; then
	echo "{\"BackendState\": \"$STATE\", \"Self\": {\"Online\": $ONLINE}}"
	exit 0
fi
for arg in "$@"; do
	case "$arg" in
	--auth-key=file:*)
		path=${arg#--auth-key=file:}
		echo "$path" > "$LOG.keypath"
		cat "$path" > "$LOG.keyfile"
		stat -c %a "$path" > "$LOG.keymode"
		;;
	esac
done
exit "$UP_STATUS"
"""


@pytest.fixture
def run(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "tailscale").write_text(TAILSCALE)
    (bin_dir / "sudo").write_text('#!/usr/bin/env bash\nexec "$@"\n')
    for f in bin_dir.iterdir():
        f.chmod(0o755)
    log = tmp_path / "log"

    def go(key=KEY, state="NeedsLogin", up_status=0, online=False):
        env = {"PATH": f"{bin_dir}:{os.environ['PATH']}", "LOG": str(log),
               "STATE": state, "UP_STATUS": str(up_status),
               "ONLINE": "true" if online else "false",
               "CODESPACE_NAME": "test-space", "TMPDIR": str(tmp_path)}
        if key is not None:
            env["TS_AUTH_KEY"] = key
        result = subprocess.run(["bash", str(SCRIPT)], env=env,
                                capture_output=True, text=True)
        calls = log.read_text().splitlines() if log.exists() else []
        return result, calls, log

    return go


def test_without_the_secret_it_does_nothing(run):
    result, calls, _ = run(key=None)
    assert result.returncode == 0
    assert "no TS_AUTH_KEY" in result.stdout
    assert calls == []


def test_already_on_the_tailnet_it_does_nothing_more(run):
    result, calls, _ = run(state="Running", online=True)
    assert result.returncode == 0
    assert "already on the tailnet" in result.stdout
    assert not any(c.startswith("up") for c in calls)


def test_a_running_daemon_that_never_joined_still_joins(run):
    # The Tailscale feature starts tailscaled before the Codespace secrets
    # exist, so it sits logged out with BackendState "Running" and no
    # tailnet. Reading that as success left Codespaces unable to reach the
    # flatsat while this script reported everything was fine.
    result, calls, _ = run(state="Running", online=False)
    assert result.returncode == 0
    assert "already on the tailnet" not in result.stdout
    assert [c for c in calls if c.startswith("up")], "it must try to join"


def test_joins_with_the_key_from_a_file_never_the_command_line(run):
    result, calls, log = run()
    assert result.returncode == 0
    up = [c for c in calls if c.startswith("up")]
    assert len(up) == 1
    assert KEY not in "\n".join(calls)
    assert Path(f"{log}.keyfile").read_text() == KEY


def test_join_has_a_time_limit_and_the_codespace_name(run):
    _, calls, _ = run()
    up = next(c for c in calls if c.startswith("up"))
    assert "--timeout=60s" in up.split()
    assert "--hostname=test-space" in up.split()
    assert "--accept-routes" in up.split()


def test_key_file_is_private_and_removed_after(run):
    _, _, log = run()
    assert Path(f"{log}.keymode").read_text().strip() == "600"
    path = Path(Path(f"{log}.keypath").read_text().strip())
    assert not path.exists()


def test_a_failed_join_warns_but_never_fails_the_start(run):
    result, _, log = run(up_status=1)
    assert result.returncode == 0
    assert "couldn't join the tailnet" in result.stderr
    path = Path(Path(f"{log}.keypath").read_text().strip())
    assert not path.exists()
