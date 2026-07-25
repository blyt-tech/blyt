#!/usr/bin/env bash
# scripts/release-dep.sh <dep> <tag>
#
# Standard release helper for blyt-tech fork deps whose GitHub auto-generated
# tarball is sufficient (currently: musl).
#
# Usage:
#   scripts/release-dep.sh musl v1.2.6-blyt-v0-p1
#
# Prerequisites:
#   - third_party/<dep> must be a populated clone of the blyt-tech fork
#   - gh CLI must be authenticated (gh auth login)
#   - The blyt-patches-v0 branch in third_party/<dep> must be at the commit
#     you want to tag
#
# After running, copy the printed FetchContent snippet into CMakeLists.txt.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <dep> <tag>" >&2
    echo "  e.g. $0 musl v1.2.6-blyt-v0-p1" >&2
    exit 1
fi

DEP="$1"
TAG="$2"
REPO_DIR="${BLYT_REPO_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
DEP_DIR="${BLYT_DEP_DIR:-${REPO_DIR}/third_party/${DEP}}"

if ! git -C "${DEP_DIR}" rev-parse --git-dir > /dev/null 2>&1; then
    echo "Error: ${DEP_DIR} is not a git repository." >&2
    echo "Clone the blyt-tech/${DEP} fork there first:" >&2
    echo "  git clone https://github.com/blyt-tech/${DEP} third_party/${DEP}" >&2
    exit 1
fi

# Determine the remote (blyt-tech fork)
REMOTE_URL=$(git -C "${DEP_DIR}" remote get-url origin 2>/dev/null || true)
if [[ -z "${REMOTE_URL}" ]]; then
    echo "Error: could not determine remote URL for third_party/${DEP}" >&2
    exit 1
fi

# Extract owner/repo from the remote URL for gh CLI
REPO_SLUG=$(echo "${REMOTE_URL}" | sed -E 's|.*github\.com[/:]([^/]+/[^/.]+)(\.git)?$|\1|')

echo "==> Tagging ${TAG} in third_party/${DEP} (${REPO_SLUG})"
git -C "${DEP_DIR}" tag "${TAG}"
git -C "${DEP_DIR}" push origin "${TAG}"

echo "==> Creating GitHub release ${TAG}"
gh release create "${TAG}" \
    --repo "${REPO_SLUG}" \
    --title "${TAG}" \
    --notes "blyt-specific patch set. See blyt-tech/blyt docs/contributing/third-party.md."

# GitHub auto-generates a tarball for every tag at a well-known URL.
TARBALL_URL="https://github.com/${REPO_SLUG}/archive/refs/tags/${TAG}.tar.gz"

echo "==> Computing SHA256 of ${TARBALL_URL}"
# macOS ships sha256sum in /sbin; older images only have shasum.
if command -v sha256sum > /dev/null 2>&1; then
    SHA256=$(curl -fsSL "${TARBALL_URL}" | sha256sum | awk '{print $1}')
else
    SHA256=$(curl -fsSL "${TARBALL_URL}" | shasum -a 256 | awk '{print $1}')
fi

# ${DEP^^} is a bash 4 expansion; macOS ships bash 3.2, where it is a fatal
# "bad substitution" — and it fires *after* the tag and release are already
# pushed, so the script would exit 1 on work that actually succeeded.
DEP_UPPER=$(printf '%s' "${DEP}" | tr '[:lower:]' '[:upper:]')

echo ""
echo "=== CMakeLists.txt snippet ==="
echo ""
echo "set(${DEP_UPPER}_VERSION \"${TAG}\")"
echo "FetchContent_Declare("
echo "  ${DEP}"
echo "  URL \"https://github.com/${REPO_SLUG}/archive/refs/tags/\${${DEP_UPPER}_VERSION}.tar.gz\""
echo "  URL_HASH SHA256=${SHA256})"
echo ""
