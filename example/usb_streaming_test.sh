#!/bin/sh
# Simple USB streaming test
# Usage: ./usb_streaming_test.sh [/dev/ttyACM0]

set -eu

DEV="${1:-}"
MODE=""

if [ -z "$DEV" ]; then
	if ls /dev/ttyACM* >/dev/null 2>&1; then
		DEV=$(ls /dev/ttyACM* | head -n1)
		MODE="cdc"
	elif ls /dev/hidraw* >/dev/null 2>&1; then
		DEV=$(ls /dev/hidraw* | head -n1)
		MODE="hid"
	else
		echo "No /dev/ttyACM* or /dev/hidraw* found. Pass device path explicitly." >&2
		exit 1
	fi
fi

case "$DEV" in
	/dev/ttyACM*) MODE="${MODE:-cdc}" ;;
	/dev/hidraw*) MODE="${MODE:-hid}" ;;
	*) MODE="${MODE:-cdc}" ;;
esac

if [ "$MODE" = "cdc" ]; then
	stty -F "$DEV" raw -echo -ixon -ixoff -crtscts
fi

# Enter streaming mode and send one frame (44 columns x 16-bit)
send_frame() {
	# Enter streaming mode
	printf '\x02\x00' > "$DEV"

	# Build payload in a temp buffer
	payload=""
	payload="$payload\x03"
	i=0
	while [ "$i" -lt 44 ]; do
		if [ $((i % 2)) -eq 0 ]; then
			payload="$payload\xff\x07"
		else
			payload="$payload\x00\x00"
		fi
		i=$((i + 1))
	done

	if [ "$MODE" = "hid" ]; then
		# Send in two chunks: 64 bytes then remaining 25 bytes
		printf "$payload" | dd of="$DEV" bs=1 count=64 conv=notrunc status=none
		printf "$payload" | dd of="$DEV" bs=1 skip=64 count=25 conv=notrunc status=none
	else
		printf "$payload" > "$DEV"
	fi
}

send_frame

sleep 1

# Leave streaming mode
printf '\x02\x01' > "$DEV"
