#!/var/jb/bin/sh
set -eu
umask 077
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
export PATH

DIR=/var/jb/etc/ssh/switch
CONFIG="$DIR/sshd_config"
HOST_KEY="$DIR/ssh_host_ed25519_key"
AUTH_DST="$DIR/authorized_keys"
AUTH_SRC=/private/var/Switch/ssh/authorized_keys

# Under launchd inetd, stdout is the accepted TCP socket: keep helper output off stdout.
if [ -L "$DIR" ]; then
    exit 1
fi
mkdir -p "$DIR" >/dev/null 2>&1
chown 0:0 "$DIR" >/dev/null 2>&1
chmod 0700 "$DIR" >/dev/null 2>&1

if [ -L "$HOST_KEY" ]; then
    exit 1
fi
if [ ! -f "$HOST_KEY" ]; then
    tmp_key="$DIR/.ssh_host_ed25519_key.tmp.$$"
    rm -f "$tmp_key" "${tmp_key}.pub" >/dev/null 2>&1
    if ! /var/jb/usr/bin/ssh-keygen -q -t ed25519 -N '' -f "$tmp_key" >/dev/null 2>&1; then
        rm -f "$tmp_key" "${tmp_key}.pub" >/dev/null 2>&1
        exit 1
    fi
    chown 0:0 "$tmp_key" "${tmp_key}.pub" >/dev/null 2>&1
    chmod 0600 "$tmp_key" >/dev/null 2>&1
    chmod 0644 "${tmp_key}.pub" >/dev/null 2>&1
    # link(2) fails with EEXIST if a concurrent connection already installed the host key.
    if ln "$tmp_key" "$HOST_KEY" >/dev/null 2>&1; then
        mv -f "${tmp_key}.pub" "${HOST_KEY}.pub" >/dev/null 2>&1 || rm -f "${tmp_key}.pub" >/dev/null 2>&1
    else
        rm -f "${tmp_key}.pub" >/dev/null 2>&1
    fi
    rm -f "$tmp_key" >/dev/null 2>&1
fi

tmp_auth="$DIR/.authorized_keys.tmp.$$"
rm -f "$tmp_auth" >/dev/null 2>&1
if [ ! -L "$AUTH_SRC" ] && [ -f "$AUTH_SRC" ]; then
    if grep -E '^(ssh-(ed25519|rsa|dss)|ecdsa-sha2-nistp(256|384|521)|sk-(ssh-ed25519|ecdsa-sha2-nistp256)@openssh\.com)[[:space:]]+[A-Za-z0-9+/]+={0,2}([[:space:]].*)?$' "$AUTH_SRC" >"$tmp_auth" 2>/dev/null && [ -s "$tmp_auth" ]; then
        chown 0:0 "$tmp_auth" >/dev/null 2>&1
        chmod 0600 "$tmp_auth" >/dev/null 2>&1
        mv -f "$tmp_auth" "$AUTH_DST" >/dev/null 2>&1
    else
        rm -f "$tmp_auth" "$AUTH_DST" >/dev/null 2>&1
    fi
else
    rm -f "$AUTH_DST" >/dev/null 2>&1
fi

exec /var/jb/usr/sbin/sshd -i -f "$CONFIG"
