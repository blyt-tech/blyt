#!/usr/bin/env bash
# scripts/release-rv32emu.sh <tag>
#
# Creates a custom rv32emu release tarball with the softfloat nested submodule
# embedded (GitHub's auto-generated tarball omits it since it's a git submodule).
#
# Usage:
#   scripts/release-rv32emu.sh g044cdb7-blyt-v0-p3
#
# Prerequisites:
#   - third_party/rv32emu must be a populated clone of blyt-tech/rv32emu
#   - third_party/rv32emu/src/softfloat must be initialised:
#       git -C third_party/rv32emu submodule update --init src/softfloat
#   - gh CLI must be authenticated (gh auth login)
#   - The blyt-patches-v0 branch must be at the commit you want to tag
#
# After running, copy the printed FetchContent snippet into CMakeLists.txt.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <tag>" >&2
    echo "  e.g. $0 g044cdb7-blyt-v0-p3" >&2
    exit 1
fi

TAG="$1"
REPO_DIR="${BLYT_REPO_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
RV32EMU_DIR="${BLYT_DEP_DIR:-${REPO_DIR}/third_party/rv32emu}"
REPO_SLUG="blyt-tech/rv32emu"

if ! git -C "${RV32EMU_DIR}" rev-parse --git-dir > /dev/null 2>&1; then
    echo "Error: ${RV32EMU_DIR} is not a git repository." >&2
    echo "Clone blyt-tech/rv32emu there first:" >&2
    echo "  git clone https://github.com/blyt-tech/rv32emu third_party/rv32emu" >&2
    exit 1
fi

if [[ ! -f "${RV32EMU_DIR}/src/softfloat/source/f64_add.c" ]]; then
    echo "Error: softfloat submodule is not initialised in third_party/rv32emu." >&2
    echo "Run: git -C third_party/rv32emu submodule update --init src/softfloat" >&2
    exit 1
fi

WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT

TARBALL_NAME="rv32emu-${TAG}.tar.gz"
TARBALL_PATH="${WORK_DIR}/${TARBALL_NAME}"

echo "==> Tagging ${TAG} in third_party/rv32emu"
git -C "${RV32EMU_DIR}" tag "${TAG}"
git -C "${RV32EMU_DIR}" push origin "${TAG}"

echo "==> Creating tarball with softfloat embedded: ${TARBALL_NAME}"
# Archive the rv32emu tree from the tag, stripping the top-level directory
# so the tarball root is rv32emu's root (not rv32emu-<tag>/).
git -C "${RV32EMU_DIR}" archive --format=tar --prefix="" "${TAG}" \
    | tar -x -C "${WORK_DIR}" -f -
# git archive leaves an empty src/softfloat/ stub for the submodule gitlink.
# Remove it before copying so cp -r places softfloat AT src/softfloat, not
# inside it (cp -r src DST/existing-dir copies src INTO DST/existing-dir).
rm -rf "${WORK_DIR}/src/softfloat"
# COPYFILE_DISABLE prevents macOS cp from creating ._* resource-fork sidecars
# and from copying extended attributes to the destination.
COPYFILE_DISABLE=1 cp -r "${RV32EMU_DIR}/src/softfloat" "${WORK_DIR}/src/softfloat"
# Strip all extended attributes (e.g. com.apple.provenance) from the work tree
# before packing. Without this, macOS tar embeds xattrs as LIBARCHIVE.xattr.*
# PAX headers; libarchive on Linux then extracts them as ._* AppleDouble files,
# which clang tries to compile as C source. xattr is macOS-only; Linux runners
# never have these attributes so the stripping step is a no-op there.
if [[ "$(uname)" == "Darwin" ]]; then
    xattr -rc "${WORK_DIR}"
fi
# Re-pack as .tar.gz. COPYFILE_DISABLE=1 additionally tells macOS tar (bsdtar)
# not to include Mac-specific metadata in the archive.
COPYFILE_DISABLE=1 tar -C "${WORK_DIR}" -czf "${TARBALL_PATH}" \
    --exclude="./${TARBALL_NAME}" \
    .

echo "==> Creating GitHub release and uploading tarball"
gh release create "${TAG}" \
    --repo "${REPO_SLUG}" \
    --title "${TAG}" \
    --notes "rv32emu with embedded softfloat submodule. See blyt-tech/blyt docs/contributing/third-party.md." \
    "${TARBALL_PATH}"

TARBALL_URL="https://github.com/${REPO_SLUG}/releases/download/${TAG}/${TARBALL_NAME}"

echo "==> Computing SHA256 of uploaded tarball"
SHA256=$(sha256sum "${TARBALL_PATH}" | awk '{print $1}')

echo ""
echo "=== CMakeLists.txt snippet ==="
echo ""
echo "set(RV32EMU_VERSION \"${TAG}\")"
echo "FetchContent_Declare("
echo "  rv32emu"
echo "  URL \"https://github.com/${REPO_SLUG}/releases/download/\${RV32EMU_VERSION}/rv32emu-\${RV32EMU_VERSION}.tar.gz\""
echo "  URL_HASH SHA256=${SHA256})"
echo ""
