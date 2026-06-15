#!/usr/bin/env bash
# scripts/release-libcxx.sh <tag>
#
# Creates a curated libc++ tarball containing only the libcxx/ and libcxxabi/
# subtrees from the blyt-tech/llvm-project fork. The full llvm-project repo is
# hundreds of MB; this script extracts only the parts the blyt build needs.
#
# The tarball root corresponds to the llvm-project root (i.e.
# ${libcxx_SOURCE_DIR}/runtimes/CMakeLists.txt, libcxx/src, etc. all exist at
# expected paths). cmake/blyt_sdk.cmake configures against
# ${libcxx_SOURCE_DIR}/runtimes so this layout must be preserved.
#
# Usage:
#   scripts/release-libcxx.sh v22.1.5-blyt-v0-p5
#
# Prerequisites:
#   - third_party/libcxx must be a populated clone of blyt-tech/llvm-project
#   - gh CLI must be authenticated (gh auth login)
#   - The blyt-patches-v0 branch must be at the commit you want to tag
#
# After running, copy the printed FetchContent snippet into CMakeLists.txt.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <tag>" >&2
    echo "  e.g. $0 v22.1.5-blyt-v0-p5" >&2
    exit 1
fi

TAG="$1"
REPO_DIR="${BLYT_REPO_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
LIBCXX_DIR="${BLYT_DEP_DIR:-${REPO_DIR}/third_party/libcxx}"
REPO_SLUG="blyt-tech/llvm-project"

if ! git -C "${LIBCXX_DIR}" rev-parse --git-dir > /dev/null 2>&1; then
    echo "Error: ${LIBCXX_DIR} is not a git repository." >&2
    echo "Clone blyt-tech/llvm-project there first:" >&2
    echo "  git clone https://github.com/blyt-tech/llvm-project third_party/libcxx" >&2
    exit 1
fi

if [[ ! -f "${LIBCXX_DIR}/runtimes/CMakeLists.txt" ]]; then
    echo "Error: ${LIBCXX_DIR}/runtimes/CMakeLists.txt not found." >&2
    echo "Is third_party/libcxx checked out on the correct branch?" >&2
    exit 1
fi

WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT

TARBALL_NAME="libcxx-${TAG}.tar.gz"
TARBALL_PATH="${WORK_DIR}/${TARBALL_NAME}"
STAGE_DIR="${WORK_DIR}/stage"

echo "==> Tagging ${TAG} in third_party/libcxx"
git -C "${LIBCXX_DIR}" tag "${TAG}"
git -C "${LIBCXX_DIR}" push origin "${TAG}"

echo "==> Extracting libcxx/libcxxabi/libc(partial)/runtimes subtrees"
mkdir -p "${STAGE_DIR}"

# Subtrees needed by cmake/blyt_sdk.cmake and cmake/blyt_guest_libs.cmake:
#   runtimes/           CMake configure entry point for libcxx+libcxxabi
#   libcxx/src          libc++ implementation sources
#   libcxx/include      libc++ headers
#   libcxxabi/src       libc++abi implementation sources
#   libcxxabi/include   libc++abi headers
#   libc/src/__support  utility headers pulled in by libcxx
#   libc/shared         shared utility headers
#   cmake/              LLVM CMake modules (LLVMVersion.cmake etc.)
#   llvm/cmake/modules/ AddLLVM, HandleLLVMOptions, GetHostTriple — required
#                       by runtimes/CMakeLists.txt via hard-coded relative path
#                       "${CMAKE_CURRENT_SOURCE_DIR}/../llvm/cmake/modules"
for subtree in runtimes libcxx libcxxabi cmake; do
    if [[ -d "${LIBCXX_DIR}/${subtree}" ]]; then
        cp -r "${LIBCXX_DIR}/${subtree}" "${STAGE_DIR}/${subtree}"
    fi
done
if [[ -d "${LIBCXX_DIR}/llvm/cmake" ]]; then
    mkdir -p "${STAGE_DIR}/llvm"
    cp -r "${LIBCXX_DIR}/llvm/cmake" "${STAGE_DIR}/llvm/cmake"
fi

# libc/ partial: only __support and shared headers
if [[ -d "${LIBCXX_DIR}/libc" ]]; then
    mkdir -p "${STAGE_DIR}/libc/src"
    if [[ -d "${LIBCXX_DIR}/libc/src/__support" ]]; then
        cp -r "${LIBCXX_DIR}/libc/src/__support" "${STAGE_DIR}/libc/src/__support"
    fi
    if [[ -d "${LIBCXX_DIR}/libc/shared" ]]; then
        cp -r "${LIBCXX_DIR}/libc/shared" "${STAGE_DIR}/libc/shared"
    fi
fi

# Licence
if [[ -f "${LIBCXX_DIR}/LICENSE.TXT" ]]; then
    cp "${LIBCXX_DIR}/LICENSE.TXT" "${STAGE_DIR}/LICENSE.TXT"
fi

echo "==> Packing ${TARBALL_NAME}"
tar -C "${STAGE_DIR}" -czf "${TARBALL_PATH}" .

echo "==> Creating GitHub release and uploading tarball"
gh release create "${TAG}" \
    --repo "${REPO_SLUG}" \
    --title "${TAG}" \
    --notes "Curated libcxx/libcxxabi subtree tarball. See blyt-tech/blyt docs/contributing/third-party.md." \
    "${TARBALL_PATH}"

echo "==> Computing SHA256"
SHA256=$(sha256sum "${TARBALL_PATH}" | awk '{print $1}')

echo ""
echo "=== CMakeLists.txt snippet ==="
echo ""
echo "set(LIBCXX_VERSION \"${TAG}\")"
echo "FetchContent_Declare("
echo "  libcxx"
echo "  URL \"https://github.com/${REPO_SLUG}/releases/download/\${LIBCXX_VERSION}/libcxx-\${LIBCXX_VERSION}.tar.gz\""
echo "  URL_HASH SHA256=${SHA256})"
echo ""
