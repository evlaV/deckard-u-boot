#!/bin/bash
#
# Build U-Boot proper for deckard (build-deckard/u-boot.img) in holo's build
# container, with the same settings as holo's deckard-uboot package.
#
#   board/qualcomm/deckard/build.sh          incremental build
#   board/qualcomm/deckard/build.sh clean    build from scratch
#
# I highly recommend installing crossdirect from holo for this.

set -euo pipefail

# Inside the container with crossdirect, set it up before anything else: its
# wrapped utilities (even the `dirname` just below) need the native dynamic
# linker set up first, like in holo's build_package.sh.
if [[ -f /var/.containerized ]] && [[ -n "${CROSSDIRECT:-}" ]]; then
    sudo "$(which setup_crossdirect)"
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${REPO_DIR}/build-deckard"
IMAGE="registry.gitlab.steamos.cloud/deckard/deckardos/holo/steamos-arm64-package-builder-main:latest"

# Enter the build container if we aren't already in it
if [[ ! -f /var/.containerized ]]; then
    PODMAN_OPTS=(
        --rm

        # The image is arm64; on an x86_64 host it runs under qemu
        --platform linux/arm64

        -v "${REPO_DIR}:${REPO_DIR}"
        -w "${REPO_DIR}"

        # Keep a cache of packages and package databases
        -v "${REPO_DIR}/.cache/pacman:/var/cache/pacman/pkg:z"
        -v "${REPO_DIR}/.cache/pacman-db:/var/lib/pacman/sync:z"

        # Map our current user in as `builder`
        --userns "keep-id:uid=1000,gid=1000"

        # Don't log everything to journald
        --log-driver=none
    )

    # If the host has crossdirect, use its cross compiler instead of emulating
    # a native one, the same way as holo's container.sh
    if [[ -d /usr/crossdirect ]]; then
        PODMAN_OPTS+=(
            -v "/usr:/native/usr"
            -v "/bin:/native/bin"
            -v "/sbin:/native/sbin"
            -v "/lib:/native/lib"
            -v "/lib64:/native/lib64"
            -v "/etc:/native/etc"
            -e "PATH=/native/usr/crossdirect:/usr/bin"
            -e "CROSSDIRECT=true"
        )
    fi
    if [[ -t 1 ]]; then
        PODMAN_OPTS+=( -ti )
    fi
    mkdir -p "${REPO_DIR}/.cache/pacman" "${REPO_DIR}/.cache/pacman-db"
    exec podman run "${PODMAN_OPTS[@]}" "${IMAGE}" "${SCRIPT_DIR}/build.sh" "$@"
fi

# The deckard-uboot package's makedepends that the build uses. The image has
# no package databases, so fetch them first, like holo's build_package.sh.
sudo pacman -Sy --needed --noconfirm git openssl python

if [[ "${1:-}" == "clean" ]]; then
    rm -rf "${BUILD_DIR}"
fi
mkdir -p "${BUILD_DIR}"

# Reproducible, like holo's build
SOURCE_DATE_EPOCH="$(git -C "${REPO_DIR}" log -1 --format=%ct)"
export SOURCE_DATE_EPOCH

# Default environment, the same as holo's deckard-uboot build()
cat > "${BUILD_DIR}/uboot-env.txt" <<'EOF'
bootdelay=2
verify=n
default_bootargs=loglevel=3 rd.log=console rd.log=file
EOF

MAKE_FLAGS=(
    -C "${REPO_DIR}"
    O="${BUILD_DIR}"
    ARCH=arm
    CONFIG_USE_DEFAULT_ENV_FILE=y
    CONFIG_DEFAULT_ENV_FILE="uboot-env.txt"
)
# With crossdirect, the compiler is the host's cross compiler
if [[ -n "${CROSSDIRECT:-}" ]]; then
    MAKE_FLAGS+=( CROSS_COMPILE=aarch64-linux-gnu- )
fi
make "${MAKE_FLAGS[@]}" sm8650-deckard_defconfig
make "${MAKE_FLAGS[@]}" -j"$(nproc)"

echo "Built ${BUILD_DIR}/u-boot.img"
