#!/usr/bin/env bash
# Starts a real SSH server for the P1-A7 manual verification (and for the CI leg that
# needs one).
#
# The point is that the server the user verifies against is the same server the CI
# verifies against: this script is the single source for both, so the parameters the CI
# hands to the tests are exactly the ones the script prints for a human.
#
# It starts sshd on 127.0.0.1:2222 with
#   * a generated ed25519 key for the current user (public-key authentication), and
#   * a throwaway account with a password (password authentication, the path the Quick
#     Connect dialog uses).
#
# Nothing outside a temporary directory and the throwaway account is touched. The account
# can be removed afterwards with:
#   sudo userdel --remove openscrt-pw
#
# Usage:
#   tools/local_sshd.sh              start the server and print how to reach it
#   tools/local_sshd.sh --export-env start it and write the CI variables to $GITHUB_ENV
#   tools/local_sshd.sh --stop       stop the server and remove the throwaway account

set -euo pipefail

PORT="${OPENCRT_TEST_SSH_PORT:-2222}"
KEY_DIR="${OPENCRT_TEST_SSH_KEY_DIR:-${TMPDIR:-/tmp}/openscrt-sshd}"
KEY_PATH="$KEY_DIR/openscrt_test_key"
PASSWORD_USER="openscrt-pw"
LOG_FILE="$KEY_DIR/sshd.log"

log() { printf '%s\n' "$*"; }

fail() {
    printf '::error::%s\n' "$*" >&2
    exit 1
}

stop_server() {
    if [ -f "$KEY_DIR/sshd.pid" ]; then
        local pid
        pid="$(cat "$KEY_DIR/sshd.pid")"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
        rm -f "$KEY_DIR/sshd.pid"
    fi
    if command -v pkill >/dev/null 2>&1; then
        pkill -f "sshd.*$KEY_DIR/sshd_config" 2>/dev/null || true
    fi
    sudo userdel --remove "$PASSWORD_USER" 2>/dev/null || true
    log "stopped the test server and removed $PASSWORD_USER"
}

if [ "${1:-}" = "--stop" ]; then
    stop_server
    exit 0
fi

if ! command -v sudo >/dev/null 2>&1; then
    fail "sudo is required: the server has to be able to change the user it logs in as"
fi

# 1. The packages and the host key.
if [ "$(uname -s)" = "Darwin" ]; then
    # macOS ships sshd; the host key may not exist yet.
    sudo mkdir -p /etc/ssh
    [ -f /etc/ssh/ssh_host_ed25519_key ] || sudo ssh-keygen -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key -q
else
    if ! command -v sshd >/dev/null 2>&1 && [ ! -x /usr/sbin/sshd ]; then
        sudo apt-get update -qq
        sudo apt-get install -y -qq openssh-server
    fi
    sudo mkdir -p /run/sshd
    sudo ssh-keygen -A
fi

# 2. The key the user logs in with.
if [ -n "$KEY_DIR" ] && [ -d "$KEY_DIR" ]; then
    rm -rf "$KEY_DIR"
fi
mkdir -p "$KEY_DIR"
chmod 700 "$KEY_DIR"
ssh-keygen -t ed25519 -N '' -f "$KEY_PATH" -q
cp "$KEY_PATH.pub" "$KEY_DIR/authorized_keys"
# The ssh.com (VanDyke) key the P1-T11 test uses, so the conversion is proved end to end:
# the server accepts the key that our parser produced from the ssh.com file, not just a
# key libssh already understood.
FIXTURE="$(cd "$(dirname "$0")/.." && pwd)/tests/fixtures/ssh/sshcom_rsa_plain.pub"
if [ -f "$FIXTURE" ]; then
    printf 'ssh-rsa %s openscrt-sshcom-fixture\n' "$(cat "$FIXTURE")" >> "$KEY_DIR/authorized_keys"
fi
chmod 600 "$KEY_DIR/authorized_keys"

# 3. The throwaway account with a password. The password is generated here and never
#    leaves this machine except through the variables below.
PASSWORD_VALUE="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
sudo useradd --create-home --shell /bin/bash "$PASSWORD_USER" 2>/dev/null || true
echo "$PASSWORD_USER:$PASSWORD_VALUE" | sudo chpasswd

# 4. The configuration. StrictModes is off because the key directory is not root-owned,
#    and PasswordAuthentication is on because the Quick Connect password path has to be
#    reachable.
SSHD_CONFIG="$KEY_DIR/sshd_config"
cat > "$SSHD_CONFIG" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey /etc/ssh/ssh_host_ed25519_key
AuthorizedKeysFile $KEY_DIR/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication yes
KbdInteractiveAuthentication no
UsePAM yes
PermitRootLogin no
StrictModes no
UseDNS no
LogLevel VERBOSE
EOF

# 5. Start it and wait for the port to answer.
sudo /usr/sbin/sshd -f "$SSHD_CONFIG" -E "$LOG_FILE" || {
    fail "sshd refused to start"
    cat "$LOG_FILE" || true
}

for _ in $(seq 1 30); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
        exec 3>&-
        break
    fi
    sleep 1
done
if ! (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    fail "sshd did not answer on port $PORT"
    cat "$LOG_FILE" || true
fi

# 6. Where the runner finds it, and where a human reads it.
if [ "${1:-}" = "--export-env" ] && [ -n "${GITHUB_ENV:-}" ]; then
    {
        echo "OPENCRT_TEST_SSH_PORT=$PORT"
        echo "OPENCRT_TEST_SSH_USER=$(id -un)"
        echo "OPENCRT_TEST_SSH_KEY=$KEY_PATH"
        echo "OPENCRT_TEST_SSH_PASSWORD_USER=$PASSWORD_USER"
        echo "OPENCRT_TEST_SSH_PASSWORD=$PASSWORD_VALUE"
    } >> "$GITHUB_ENV"
fi

log ""
log "A real SSH server is listening on 127.0.0.1:$PORT"
log ""
log "  public key : $KEY_PATH   (user: $(id -un))"
log "  password   : user $PASSWORD_USER, password $PASSWORD_VALUE"
log ""
log "In openSCRT: File -> New Session (Quick Connect)..., then"
log "  host 127.0.0.1, port $PORT, and either the key above or the password above."
log ""
log "Stop it with: tools/local_sshd.sh --stop"
