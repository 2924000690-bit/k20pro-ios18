#!/system/bin/sh
# 一键：下载 iOS18 recovery 镜像 -> 校验 -> 写入 recovery -> 重启进 recovery
#
#   su -c "sh /sdcard/Download/flash-rec-auto.sh"
#   su -c "sh /sdcard/Download/flash-rec-auto.sh --no-reboot"   # 只刷不重启
#
# 镜像来源是 GitHub release，脚本自己下载并校验 sha256 后再写，写坏会中止。
set -e

URL=${IMG_URL:-https://github.com/Gsjsjzhznsz/UniXNU-raphael-iOS18-K20Pro/releases/download/rec-v2-20261120/rec-ios18-skywalk-uc-v2.img}
SHA=9d582cf8995518977ff477bd6dab86e067963f0a3073025f944dc6cc568ed276
SIZE=67108864
REBOOT=1
[ "$1" = "--no-reboot" ] && REBOOT=0

if [ "$(id -u)" != "0" ]; then
	SELF=$(readlink -f "$0" 2>/dev/null || echo "$0")
	echo "需要 root，正在提权 ..."
	exec su -c "sh '$SELF' $*"
fi

if [ -w /sdcard/Download ]; then DIR=/sdcard/Download; else DIR=/data/local/tmp; fi
IMG=$DIR/rec-ios18-skywalk-uc-v2.img

size_of() { wc -c < "$1" 2>/dev/null | tr -d ' '; }

echo "镜像地址: $URL"
echo "保存到  : $IMG"

if [ -f "$IMG" ] && [ "$(size_of "$IMG")" = "$SIZE" ]; then
	echo "已有完整镜像，跳过下载"
else
	echo "下载中（约 64 MiB）..."
	if command -v curl >/dev/null 2>&1; then
		curl -L --fail --retry 3 -o "$IMG.part" "$URL"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$IMG.part" "$URL"
	elif command -v busybox >/dev/null 2>&1; then
		busybox wget -O "$IMG.part" "$URL"
	else
		echo "没有 curl/wget/busybox，无法下载"; exit 1
	fi
	mv -f "$IMG.part" "$IMG"
fi

if [ "$(size_of "$IMG")" != "$SIZE" ]; then
	echo "镜像大小不对：$(size_of "$IMG") != $SIZE"; exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
	GOT=$(sha256sum "$IMG" | cut -d' ' -f1)
	[ "$GOT" = "$SHA" ] || { echo "sha256 不匹配：$GOT"; exit 1; }
	echo "sha256 校验通过"
else
	echo "没有 sha256sum，只校验了大小"
fi

PART=""
for cand in /dev/block/by-name/recovery /dev/block/bootdevice/by-name/recovery; do
	[ -e "$cand" ] && { PART=$cand; break; }
done
[ -n "$PART" ] || { echo "找不到 recovery 分区"; exit 1; }
PARTSIZE=$(blockdev --getsize64 "$PART" 2>/dev/null || true)
echo "分区    : $PART ($PARTSIZE 字节)"
[ -z "$PARTSIZE" ] || [ "$PARTSIZE" = "$SIZE" ] || { echo "分区大小不符，拒绝写入"; exit 1; }

echo "写入中，请勿断电 ..."
dd if="$IMG" of="$PART" bs=1048576
sync

if command -v sha256sum >/dev/null 2>&1; then
	echo "回读校验中 ..."
	B=$(dd if="$PART" bs=1048576 count=64 2>/dev/null | sha256sum | cut -d' ' -f1)
	[ "$B" = "$SHA" ] || { echo "回读校验失败：$B"; exit 1; }
	echo "回读校验通过"
fi

if [ "$REBOOT" = "1" ]; then
	echo "完成，3 秒后重启进 recovery ..."
	sleep 3
	sync
	reboot recovery || svc power reboot recovery || echo "请手动 电源+音量上 进入"
fi
echo "完成"
