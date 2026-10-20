#!/bin/bash
#
# Write U-Boot proper to a deckard device and reboot it.
#
#   board/qualcomm/deckard/deploy.sh <target>             write build-deckard/u-boot.img
#   board/qualcomm/deckard/deploy.sh <target> --restore   write the packaged /boot/uboot.img back
#
# <target> is an ssh target, e.g. frame.local or steamos@frame.local.
#
# Like `kernelsetup.sh --sameslot`, this only writes the uboot_<slot> partition
# of the slot the device booted from; SPL, bootfw and the environment are not
# touched. If the new U-Boot doesn't boot, boot the other slot and use --restore.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/../../.." && pwd)"
TARGET="${1:?usage: $0 <target> [--restore]}"

if [[ "${2:-}" == "--restore" ]]; then
    FILE="/boot/uboot.img"
else
    FILE="/tmp/uboot-deploy.img"
    scp "${REPO_DIR}/build-deckard/u-boot.img" "${TARGET}:${FILE}"
fi

# Runs as root on the device, with the image to write as $1
read -r -d '' REMOTE_SCRIPT <<'EOF' || true
set -euo pipefail
FILE="$1"
SLOT="$(sed -n 's/.*rauc\.slot=\([AB]\).*/\1/p' /proc/cmdline)"
PART="/dev/disk/by-partlabel/uboot_${SLOT,,}"
DISK="/dev/$(lsblk -no PKNAME "${PART}")"

# Like kernelsetup.sh, make the partition and its disk writable while we write
PART_RO="$(blockdev --getro "${PART}")"
DISK_RO="$(blockdev --getro "${DISK}")"
blockdev --setrw "${PART}" "${DISK}"

echo "Writing ${FILE} to ${PART} (slot ${SLOT})"
dd if="${FILE}" of="${PART}" bs=1M conv=fsync status=none

# Read it back from the device, not the page cache
echo 3 > /proc/sys/vm/drop_caches
cmp -n "$(stat -c %s "${FILE}")" "${FILE}" "${PART}"
echo "Verified ${PART}"

[[ "${PART_RO}" == 1 ]] && blockdev --setro "${PART}"
[[ "${DISK_RO}" == 1 ]] && blockdev --setro "${DISK}"

echo "Rebooting"
systemctl reboot
EOF

ssh -t "${TARGET}" sudo bash -c "$(printf '%q' "${REMOTE_SCRIPT}")" deploy "${FILE}"
