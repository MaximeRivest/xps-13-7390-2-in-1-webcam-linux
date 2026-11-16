#!/usr/bin/env bash
set -euo pipefail

modprobe ipu-bridge
modprobe int346f
modprobe ov01a10-i346f
modprobe intel-ipu4p
modprobe intel-ipu4p-isys
modprobe v4l2loopback

media=""
for _ in $(seq 1 50); do
  for candidate in /dev/media*; do
    [[ -e "$candidate" ]] || continue
    if media-ctl -d "$candidate" -p 2>/dev/null | grep -q 'Intel IPU4P'; then
      media=$candidate
      break 2
    fi
  done
  sleep 0.2
done
if [[ -z "$media" ]]; then
  echo "The IPU4P media device did not appear." >&2
  exit 1
fi

# The entity is named after the sensor's I2C bus and address, e.g.
# "ov01a10 3-0036" or "ov01a10 2-0036"; the bus number depends on
# enumeration order, so read the name from the media graph.
sensor=$(media-ctl -d "$media" -p | sed -nE 's/.*entity [0-9]+: (ov01a10[^ ]* [^ ]+) \(.*/\1/p')
if [[ -z "$sensor" || "$sensor" == *$'\n'* ]]; then
  echo "Expected exactly one OV01A10 sensor in $media." >&2
  exit 1
fi

format=SBGGR10_1X10/1280x800
media-ctl -d "$media" -V "\"$sensor\":0 [fmt:$format]"
media-ctl -d "$media" -V '"Intel IPU4P CSI2 1":0 [fmt:'"$format"']'
media-ctl -d "$media" -V '"Intel IPU4P CSI2 1":1 [fmt:'"$format"']'
media-ctl -d "$media" -l '"Intel IPU4P CSI2 1":1 -> "Intel IPU4P ISYS Capture 8":0 [1]'

raw=""
for node in /sys/class/video4linux/video*; do
  [[ -e "$node/name" ]] || continue
  if [[ $(<"$node/name") == "Intel IPU4P ISYS Capture 8" ]]; then
    raw=/dev/$(basename "$node")
    break
  fi
done
if [[ -z "$raw" ]]; then
  echo "The IPU4P capture node did not appear." >&2
  exit 1
fi

v4l2-ctl -d "$raw" --set-fmt-video=width=1280,height=800,pixelformat=pBAA
ln -sfn "$raw" /dev/video-ipu4-raw

for subdev in /dev/v4l-subdev*; do
  if v4l2-ctl -d "$subdev" --list-ctrls 2>/dev/null | grep -q analogue_gain; then
    v4l2-ctl -d "$subdev" --set-ctrl exposure=888 || true
    v4l2-ctl -d "$subdev" --set-ctrl analogue_gain=1024 || true
    v4l2-ctl -d "$subdev" --set-ctrl digital_gain=1024 || true
  fi
done

echo "Camera ready: $raw -> /dev/video57"
