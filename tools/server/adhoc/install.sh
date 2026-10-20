#!/bin/bash
# Install the ad-hoc relay on this server. Run as root, from the unpacked package:
#
#   sudo ./install.sh                 # works out the other relay by itself
#   sudo ./install.sh --peer <IP>     # or say which it is
#
# Replaces the old dedicated race server if it is installed (both want UDP
# 28500), and carries its leaderboard over.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY=217.154.211.69
BACKUP=85.215.158.219
STATE=/var/lib/sd-adhoc-relay
OLD_RECORDS=/home/sdserver/.speed-dreams-2/records.xml

[ "$(id -u)" = 0 ] || { echo "run this as root: sudo $0" >&2; exit 1; }

PEER=""
if [ "${1:-}" = "--peer" ] && [ -n "${2:-}" ]; then
    PEER="$2"
else
    MINE=" $(hostname -I 2>/dev/null) "
    if [[ "$MINE" == *" $PRIMARY "* ]]; then PEER=$BACKUP
    elif [[ "$MINE" == *" $BACKUP "* ]]; then PEER=$PRIMARY
    fi
fi
if [ -n "$PEER" ]; then
    echo "==> the other relay is $PEER (the leaderboard is kept in step with it)"
else
    echo "==> could not tell which relay this is; running without a peer."
    echo "    Rerun with --peer <other relay's IP> to keep the leaderboards in step."
fi

if systemctl list-unit-files speed-dreams-server.service 2>/dev/null | grep -q speed-dreams-server; then
    echo "==> stopping the old dedicated race server (it uses the same port)"
    systemctl disable --now speed-dreams-server.service || true
fi

if ! id sdrelay >/dev/null 2>&1; then
    echo "==> creating the sdrelay user"
    useradd --system --no-create-home --shell /usr/sbin/nologin sdrelay
fi

systemctl stop sd-adhoc-relay.service 2>/dev/null || true

echo "==> installing /usr/local/bin/sd-adhoc-relay"
install -m 0755 "$HERE/sd-adhoc-relay" /usr/local/bin/sd-adhoc-relay

install -d -o sdrelay -g sdrelay -m 0755 "$STATE"
if [ -f "$OLD_RECORDS" ] && [ ! -f "$STATE/.imported" ]; then
    if command -v python3 >/dev/null 2>&1; then
        echo "==> carrying over the old server's leaderboard"
        python3 "$HERE/import-records.py" "$OLD_RECORDS" "$STATE/records.tsv"
        chown sdrelay:sdrelay "$STATE/records.tsv"
        touch "$STATE/.imported"
    else
        echo "==> python3 is missing, so the old leaderboard was not carried over"
    fi
fi

echo "RELAY_ARGS=\"${PEER:+-P $PEER}\"" > /etc/default/sd-adhoc-relay

echo "==> installing the systemd service"
install -m 0644 "$HERE/sd-adhoc-relay.service" /etc/systemd/system/sd-adhoc-relay.service
systemctl daemon-reload
systemctl enable sd-adhoc-relay.service
systemctl restart sd-adhoc-relay.service

if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    echo "==> opening the firewall (ufw)"
    ufw allow 28500/udp
    ufw allow 28510:28573/udp
    ufw allow 28600/tcp
elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    echo "==> opening the firewall (firewalld)"
    firewall-cmd --permanent --add-port=28500/udp
    firewall-cmd --permanent --add-port=28510-28573/udp
    firewall-cmd --permanent --add-port=28600/tcp
    firewall-cmd --reload
else
    echo "==> no ufw/firewalld running here - if your hosting provider has a firewall"
    echo "    in its web panel, open UDP 28500, UDP 28510-28573 and TCP 28600 there."
fi

sleep 1
if systemctl is-active --quiet sd-adhoc-relay.service; then
    echo "==> running. Check it with:  journalctl -u sd-adhoc-relay -f"
else
    echo "!! the service did not start:" >&2
    journalctl -u sd-adhoc-relay -n 20 --no-pager >&2
    exit 1
fi
