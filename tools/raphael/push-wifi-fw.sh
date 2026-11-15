#!/bin/sh
# Put the phone's own Wi-Fi firmware where the kernel reads it:
# /usr/local/share/firmware/wifi/ on the root volume (docs/WIFI-PORT-IOS18.md, Stage C).
#
#   tools/raphael/push-wifi-fw.sh FW_DIR
#
# FW_DIR holds what was pulled from the phone's modem partition (FAT16, by-name
# "modem", image/): modem.mdt, modem.b00..b31, wlanmdsp.mbn, bd_f11cn.bin, the NV
# snapshots nv-modemst1/nv-modemst2/nv-fsg/nv-fsc, plus firmware-5.bin and
# 0-rmtfs-ram generated as repo/README.md describes. It is private device data:
# it stays out of the repository and out of any image that leaves the phone.
set -eu
[ $# -eq 1 ] || { echo "usage: $0 FW_DIR" >&2; exit 2; }
SRC=$1
KEY=${RAPHAEL_SSH_KEY:-$(cd "$(dirname "$0")/../.." && pwd)/keys/raphael_ios_ed25519}
PHONE=${RAPHAEL_PHONE:-root@192.168.7.2}
for f in modem.mdt wlanmdsp.mbn bd_f11cn.bin firmware-5.bin 0-rmtfs-ram nv-modemst1 nv-modemst2 nv-fsg nv-fsc; do
	[ -f "$SRC/$f" ] || { echo "$0: $SRC/$f is missing" >&2; exit 1; }
done
ls "$SRC"/modem.b[0-9][0-9] >/dev/null 2>&1 || { echo "$0: no modem.bNN segments in $SRC" >&2; exit 1; }

tar -C "$SRC" -cf - . | ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "$PHONE" \
	'mkdir -p /usr/local/share/firmware/wifi && cd /usr/local/share/firmware/wifi && tar -xf - && ls -l | head'
