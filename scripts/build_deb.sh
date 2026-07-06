#!/usr/bin/env bash
# ============================================================================
# scripts/build_deb.sh — Build .deb for FineVision ROS2 SDK (colcon build)
# ============================================================================
# Usage:
#   ./scripts/build_deb.sh [-d humble] [-v 0.1.0] [--prefix /opt/ros/humble]
#
# Options:
#   -d, --ros-distro DISTRO   ROS 2 distro (default: humble)
#   -v, --version VERSION     Package version (default: from package.xml)
#   -t, --build-type TYPE     CMake build type (default: Release)
#   -j, --jobs N              Parallel jobs (default: nproc)
#   --prefix PATH             Install prefix inside .deb (default: /opt/ros/$DISTRO)
#   --output-dir PATH         Output dir for .deb (default: ./artifacts)
#   --clean                   Clean before build
#
# Prerequisites: ROS 2, colcon, CMake, Ninja, dpkg-dev
# Example: ./scripts/build_deb.sh -d humble
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ROS_DISTRO="humble"
VERSION=""
BUILD_TYPE="Release"
N_JOBS=$(nproc 2>/dev/null || echo 8)
INSTALL_PREFIX=""
OUTPUT_DIR=""
CLEAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--ros-distro) ROS_DISTRO="$2"; shift 2 ;;
        -v|--version)    VERSION="$2"; shift 2 ;;
        -t|--build-type) BUILD_TYPE="$2"; shift 2 ;;
        -j|--jobs)       N_JOBS="$2"; shift 2 ;;
        --prefix)        INSTALL_PREFIX="$2"; shift 2 ;;
        --output-dir)    OUTPUT_DIR="$2"; shift 2 ;;
        --clean)         CLEAN=true; shift ;;
        -h|--help) sed -n '/^#/{/^#!/d;/^# =/d;s/^# \?//;p}' "$0"; exit 0 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/ros/${ROS_DISTRO}}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_ROOT}/artifacts}"
VERSION="${VERSION:-$(grep -oP '<version>\K[^<]+' "${REPO_ROOT}/sdk/package.xml" || echo "0.1.0")}"

ARCH="amd64"
PKG="ros-${ROS_DISTRO}-finevision"
DEB_FILE="${PKG}_${VERSION}_${ARCH}.deb"

GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
log()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
step() { echo -e "\n${BLUE}━━━ $* ━━━${NC}"; }

# ── Prerequisites ───────────────────────────────────────────────────
step "Checking prerequisites"
for cmd in cmake ninja colcon dpkg-deb; do
    command -v "$cmd" &>/dev/null || { echo "MISSING: $cmd — install first"; exit 1; }
done
# ROS 2 setup.bash is incompatible with 'set -u' — relax temporarily
_ros_source() {
    if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
        set +u
        source /opt/ros/${ROS_DISTRO}/setup.bash
        set -u
        return 0
    fi
    return 1
}

if _ros_source; then
    log "ROS 2 ${ROS_DISTRO} sourced"
else
    echo "WARN: /opt/ros/${ROS_DISTRO}/setup.bash not found"
fi
log "All tools found"

# ── Setup workspace ──────────────────────────────────────────────────
step "Setting up colcon workspace"
WS="/tmp/finevision-colcon-ws-$$"
rm -rf "${WS}"
mkdir -p "${WS}/src"
cp -a "${REPO_ROOT}/sdk" "${WS}/src/finevision"
log "Workspace: ${WS}"

# ── Build ───────────────────────────────────────────────────────────
step "colcon build (${BUILD_TYPE})"
cd "${WS}"
_ros_source 2>/dev/null || true
$CLEAN && rm -rf build/ install/ log/

colcon build \
    --packages-select finevision \
    --cmake-args \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
        -GNinja \
    --event-handlers console_direct+ \
    --parallel-workers "${N_JOBS}"

log "Build done"
find "${WS}/install/finevision" -type f 2>/dev/null | head -40 || echo "(install empty)"

# ── Package .deb ────────────────────────────────────────────────────
step "Building .deb"
DEB_ROOT="/tmp/finevision-deb-root-$$"
rm -rf "${DEB_ROOT}"
mkdir -p "${DEB_ROOT}/DEBIAN"
mkdir -p "${DEB_ROOT}${INSTALL_PREFIX}"

# Copy only the package's own files (not workspace-level setup.* etc.)
# colcon puts per-package output in install/<name>/ — use that directly
COLCON_INSTALL="${WS}/install/finevision"
if [ -d "${COLCON_INSTALL}" ]; then
    cp -a "${COLCON_INSTALL}"/* "${DEB_ROOT}${INSTALL_PREFIX}/" 2>/dev/null || true
fi

if [ -z "$(ls -A ${DEB_ROOT}${INSTALL_PREFIX} 2>/dev/null)" ]; then
    echo "ERROR: nothing to package"; exit 1
fi

echo "Package payload:"
find "${DEB_ROOT}" -type f 2>/dev/null | head -60

# control
INST_SIZE=$(du -sk "${DEB_ROOT}${INSTALL_PREFIX}" 2>/dev/null | cut -f1 || echo 1024)
printf 'Package: %s\nVersion: %s\nArchitecture: %s\nMaintainer: IWIN-FINS Lab\nInstalled-Size: %s\nSection: libs\nPriority: optional\nDescription: FineVision SDK\n High-performance C++20 robotics SDK. Installs to %s/.\n' \
    "${PKG}" "${VERSION}" "${ARCH}" "${INST_SIZE}" "${INSTALL_PREFIX}" \
    > "${DEB_ROOT}/DEBIAN/control"

# postinst
printf '#!/bin/sh\nset -e\necho "FineVision SDK %s installed to %s/"\nif [ -d %s/lib ]; then\n  echo %s/lib > /etc/ld.so.conf.d/finevision.conf\n  ldconfig || true\nfi\n' \
    "${VERSION}" "${INSTALL_PREFIX}" "${INSTALL_PREFIX}" "${INSTALL_PREFIX}" \
    > "${DEB_ROOT}/DEBIAN/postinst"
chmod +x "${DEB_ROOT}/DEBIAN/postinst"

# postrm
printf '#!/bin/sh\nset -e\nrm -f /etc/ld.so.conf.d/finevision.conf\nldconfig || true\n' \
    > "${DEB_ROOT}/DEBIAN/postrm"
chmod +x "${DEB_ROOT}/DEBIAN/postrm"

# Build .deb
mkdir -p "${OUTPUT_DIR}"
dpkg-deb --build "${DEB_ROOT}" "${OUTPUT_DIR}/${DEB_FILE}"

# ── Done ────────────────────────────────────────────────────────────
rm -rf "${WS}" "${DEB_ROOT}"

log ".deb: ${OUTPUT_DIR}/${DEB_FILE}"
ls -lh "${OUTPUT_DIR}/${DEB_FILE}"

echo ""
echo "Install globally:"
echo "  sudo dpkg -i ${OUTPUT_DIR}/${DEB_FILE}"
echo "  source /opt/ros/${ROS_DISTRO}/setup.bash"
