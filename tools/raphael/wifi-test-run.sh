#!/usr/bin/env bash
# One shot Wi-Fi probe on the installed iOS 18 root: RAM-boot the Skywalk kernel,
# capture the kernel log and report what the Wi-Fi stack did.
#
#   tools/raphael/wifi-test-run.sh [IMAGE]
#
# Default image: ../../../wifi-test/boot-ios18-skywalk-uc.img (IO80211SkywalkInterface +
# IO80211InterfaceRole=Infrastructure + the logging IO80211 user client).
# Needs: adb/fastboot from .tools/platform-tools, the ssh key, sudo for the USB gadget.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${1:-$ROOT/../wifi-test/boot-ios18-skywalk-uc.img}
KEY=$ROOT/../keys/raphael_ios_ed25519
ADB=/vol1/1000/项目/hackios/.tools/platform-tools/adb
FASTBOOT=/vol1/1000/项目/hackios/.tools/platform-tools/fastboot
LOG=/tmp/wifi-probe-$(date +%H%M%S).log
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -i $KEY root@192.168.7.2"
export HOME=${ANDROID_HOME:-/tmp/android-home} ANDROID_USER_HOME=${ANDROID_USER_HOME:-/tmp/android-home/.android}
SUDO() { printf '%s\n' "${SUDO_PASS:-Kwj13652723054}" | sudo -S -p '' "$@"; }

step() { echo "== $*"; }
usb_fix() { for n in /dev/bus/usb/003/*; do SUDO chmod 666 "$n" 2>/dev/null; done; }

test -f "$IMAGE" || { echo "no image: $IMAGE"; exit 1; }
step "image $IMAGE"
sha256sum "$IMAGE" | head -1

step "kernel log capture -> $LOG"
pkill -f 'usblink.sh log' 2>/dev/null
sleep 1
(cd "$ROOT" && setsid nohup tools/raphael/usblink.sh log "$LOG" >/dev/null 2>&1 &)
sleep 1

if ! $SSH true 2>/dev/null; then
	step "phone not on iOS ssh; assuming Android"
	$ADB kill-server >/dev/null 2>&1
	$ADB -s 45c2b4ec reboot bootloader >/dev/null 2>&1
	sleep 10
fi
usb_fix
if $FASTBOOT devices 2>/dev/null | grep -q 45c2b4ec; then
	step "fastboot boot"
	$FASTBOOT boot "$IMAGE" 2>&1 | tail -1
else
	echo "phone is not in fastboot; put it there and rerun"; exit 1
fi
for i in $(seq 1 25); do ip -br link | grep -q enx && { step "ECM at $((i*3))s"; break; }; sleep 3; done
SUDO ip link set enx02005e77ee01 up 2>/dev/null
SUDO ip addr replace 192.168.7.1/24 dev enx02005e77ee01 2>/dev/null

step "waiting for the radio and userland"
sleep 95
grep -a -E "HAKIOS IO80211|THE RADIO IS UP|sweet_wifi_step" "$LOG" | tail -10

step "wifid and the manager endpoint"
$SSH '/bin/ps ax 2>/dev/null | grep -v grep | grep wifid | head -2
/var/jb/usr/bin/launchctl print user/501 2>/dev/null | grep -i "wifi.manager"
ifconfig en0 2>/dev/null | head -2' 2>&1 | tail -6

step "user client selectors seen"
grep -a -E "HAKIOS IO80211: (user client|selector)" "$LOG" | head -30
echo "full log: $LOG"
