#!/bin/bash
# FSK Timekeeper photo finish camera on a Raspberry Pi 5 with Raspberry Pi OS (64-bit), from a
# release (the release workflow fills in REPO and VERSION):
#   curl -fsSL https://github.com/luftaquila/fsk-timekeeper/releases/latest/download/install-photofinish.sh | sudo bash
# Run again (or fsk-timekeeper-update) to change the release. Installs podman and Tailscale, this
# release's host files (files/) and camera image, joins the tailnet (a login link the first time)
# and publishes the camera API with tailscale serve. It changes only what the camera needs; the
# Ethernet port becomes the camera's.
set -euo pipefail

REPO="@REPO@"
VERSION="@VERSION@"
API=http://127.0.0.1:8080

case "$REPO$VERSION" in *@*) echo "Use install-photofinish.sh from a release." >&2; exit 1 ;; esac
[ "$(id -u)" = 0 ] || { echo "Run as root: ... | sudo bash" >&2; exit 1; }
owner="${REPO%%/*}"
image="ghcr.io/${owner,,}/fsk-timekeeper-camera:$VERSION"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export DEBIAN_FRONTEND=noninteractive

echo "== Packages"
apt-get update
apt-get install -y podman iw
if ! command -v tailscale >/dev/null; then
    curl -fsSL https://tailscale.com/install.sh | sh
fi

echo "== Camera host files ($VERSION)"
curl -fsSL "https://github.com/$REPO/releases/download/$VERSION/fsk-timekeeper-photofinish.tar.gz" \
    | tar -xz --no-same-owner -C "$tmp"
cp -r "$tmp/files/." /
chmod 600 /etc/NetworkManager/system-connections/fsk-timekeeper-camera.nmconnection
systemd-tmpfiles --create /etc/tmpfiles.d/fsk-timekeeper.conf
printf '%s\n' "$REPO" > /etc/fsk-timekeeper/repo
printf '%s\n' "$image" > /etc/fsk-timekeeper/image
if [ ! -f /etc/fsk-timekeeper/camera.conf ]; then
    cat > /etc/fsk-timekeeper/camera.conf <<'CONF'
# Console origins the camera API allows, comma-separated (default: the public console). Add null
# for the offline console file, then: sudo systemctl restart fsk-timekeeper
#FSK_TIMEKEEPER_ORIGINS=https://luftaquila.github.io,null
CONF
fi
sysctl -q -p /etc/sysctl.d/90-fsk-timekeeper.conf
udevadm control --reload
udevadm trigger --subsystem-match=tty --action=change
udevadm trigger --subsystem-match=cpu --action=change
nmcli connection reload
# Switching eth0 now would cut a session that came in over it; the next boot does it then.
if ip -4 route show default dev eth0 2>/dev/null | grep -q .; then
    echo "eth0 carries this Pi's network now: it becomes the camera's link at the next boot."
else
    # The jumbo MTU goes on as at boot: the udev rule, while the port is down. NetworkManager first
    # lets go of the port, since it puts back the MTU it had recorded when it does.
    nmcli device disconnect eth0 >/dev/null 2>&1 || true
    ip link set eth0 down 2>/dev/null || true
    udevadm trigger --action=add --subsystem-match=net --sysname-match=eth0
    udevadm settle
    nmcli connection up fsk-timekeeper-camera >/dev/null 2>&1 || true
fi
if [ -e /sys/class/net/wlan0 ]; then
    iw dev wlan0 set power_save off || true
fi

echo "== Camera image"
podman pull "$image"
systemctl daemon-reload
systemctl enable fsk-timekeeper.service
systemctl restart fsk-timekeeper.service

echo "== Tailscale"
state="$(tailscale status --json 2>/dev/null \
    | python3 -c 'import json, sys; print(json.load(sys.stdin).get("BackendState", ""))' || true)"
# The Pi's own DNS stays off Tailscale: after a long power-off its clock is behind, Tailscale's
# TLS fails, and time sync must still resolve its servers.
if [ "$state" != Running ]; then
    echo "Open the link below and log in to your tailnet."
    tailscale up --accept-dns=false
else
    tailscale set --accept-dns=false
fi
# Port 443 of this Pi's tailnet name: ours, free, or another service's (left alone).
serve="$(tailscale serve status --json 2>/dev/null || true)"
case "$(printf '%s' "$serve" | python3 -c '
import json, sys
text = sys.stdin.read().strip()
config = json.loads(text) if text else {}
api = sys.argv[1]
web = [h for hp, w in (config.get("Web") or {}).items() if hp.endswith(":443")
       for h in (w.get("Handlers") or {}).values()]
tcp = (config.get("TCP") or {}).get("443")
if not web and not tcp:
    print("free")
elif web and all(h.get("Proxy") == api for h in web):
    print("ours")
else:
    print("taken")
' "$API")" in
    free)
        tailscale serve --bg --https=443 "$API" || {
            echo "Enable MagicDNS and HTTPS certificates in your tailnet, then run this again." >&2
            exit 1
        } ;;
    ours) ;;
    *)
        echo "Port 443 of this Pi's tailnet name serves something else; the camera API was not published." >&2
        exit 1 ;;
esac

name="$(tailscale status --json | python3 -c 'import json, sys; print(json.load(sys.stdin)["Self"]["DNSName"].rstrip("."))')"
echo
echo "FSK Timekeeper camera $VERSION is running."
echo "Console: Settings > Camera Settings > Camera Pi = $name"
