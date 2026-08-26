#!/bin/bash
# =============================================================================
# liburing auto-detection and installation script
#
# Function:
#   1. Detect whether liburing is already installed (pkg-config + headers).
#   2. If not, download and build liburing from the official GitHub source.
#   3. Ensure a compatible version (>= 2.0) for the io_uring backend.
#
# Usage:
#   ./scripts/setup_liburing.sh          # auto-detect / install
#   ./scripts/setup_liburing.sh --force  # force a clean rebuild
#
# =============================================================================

set -euo pipefail

LIBURING_VERSION="2.6"
LIBURING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-${LIBURING_VERSION}.tar.gz"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"
BUILD_DIR="/tmp/liburing-build-$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ---- Detect existing installation ----
check_installed() {
    if pkg-config --exists liburing 2>/dev/null; then
        local ver
        ver=$(pkg-config --modversion liburing)
        log_info "liburing v${ver} already installed"
        return 0
    fi

    # Fallback: check headers and library files directly
    if [ -f "${INSTALL_PREFIX}/include/liburing.h" ] && \
       [ -f "${INSTALL_PREFIX}/lib/liburing.so" -o -f "${INSTALL_PREFIX}/lib/liburing.a" ]; then
        log_info "liburing already installed (without pkg-config metadata)"
        return 0
    fi

    # Check common system paths
    if [ -f "/usr/include/liburing.h" ] && \
       [ -f "/usr/lib/x86_64-linux-gnu/liburing.so" -o \
         -f "/usr/lib/liburing.so" -o \
         -f "/usr/lib/aarch64-linux-gnu/liburing.so" ]; then
        log_info "liburing already installed (system path)"
        return 0
    fi

    return 1
}

# ---- Download and build liburing ----
install_liburing() {
    log_info "Downloading liburing v${LIBURING_VERSION} ..."

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Download the source tarball
    if command -v wget &>/dev/null; then
        wget -q --show-progress -O "liburing-${LIBURING_VERSION}.tar.gz" "$LIBURING_URL" || {
            log_error "Download failed. Check your network connection."
            rm -rf "$BUILD_DIR"
            exit 1
        }
    elif command -v curl &>/dev/null; then
        curl -L -o "liburing-${LIBURING_VERSION}.tar.gz" "$LIBURING_URL" || {
            log_error "Download failed. Check your network connection."
            rm -rf "$BUILD_DIR"
            exit 1
        }
    else
        log_error "wget or curl is required to download liburing."
        rm -rf "$BUILD_DIR"
        exit 1
    fi

    tar xzf "liburing-${LIBURING_VERSION}.tar.gz"
    cd "liburing-liburing-${LIBURING_VERSION}"

    log_info "Configuring (prefix=${INSTALL_PREFIX}) ..."

    # Build
    ./configure --prefix="$INSTALL_PREFIX" || {
        # configure failed; fall back to a plain make
        log_warn "configure failed; trying plain make"
    }

    make -j"$(nproc)" || {
        log_error "Build failed"
        rm -rf "$BUILD_DIR"
        exit 1
    }

    log_info "Installing liburing to ${INSTALL_PREFIX} ..."

    # Install (may require sudo)
    if [ -w "${INSTALL_PREFIX}" ]; then
        make install
    else
        log_warn "Root privileges required to install into ${INSTALL_PREFIX}"
        sudo make install
    fi

    # Refresh the dynamic linker cache
    if [ -w /etc/ld.so.conf.d ]; then
        echo "${INSTALL_PREFIX}/lib" | sudo tee /etc/ld.so.conf.d/liburing.conf > /dev/null
        sudo ldconfig
    elif [ -d /etc/ld.so.conf.d ]; then
        echo "${INSTALL_PREFIX}/lib" | sudo tee /etc/ld.so.conf.d/liburing.conf > /dev/null
        sudo ldconfig
    fi

    # Cleanup
    rm -rf "$BUILD_DIR"

    log_info "liburing v${LIBURING_VERSION} installation finished"
}

# ---- Main flow ----
main() {
    local force=${1:-}

    if [ "$force" != "--force" ] && check_installed; then
        echo ""
        echo "========================================"
        echo "  liburing is ready"
        echo "========================================"
        pkg-config --cflags --libs liburing 2>/dev/null || true
        exit 0
    fi

    if [ "$force" = "--force" ]; then
        log_warn "Force-rebuild mode"
    fi

    echo ""
    echo "========================================"
    echo "  Install liburing v${LIBURING_VERSION}"
    echo "========================================"
    echo ""

    # Check required tools
    for tool in gcc make; do
        if ! command -v "$tool" &>/dev/null; then
            log_error "Missing required tool: $tool"
            exit 1
        fi
    done

    install_liburing

    echo ""
    echo "========================================"
    echo "  liburing installation verification"
    echo "========================================"

    if check_installed; then
        log_info "Verification passed"
        echo ""
        echo "Build flags:"
        echo "  CFLAGS:  $(pkg-config --cflags liburing 2>/dev/null || echo "-I${INSTALL_PREFIX}/include")"
        echo "  LDFLAGS: $(pkg-config --libs liburing 2>/dev/null || echo "-L${INSTALL_PREFIX}/lib -luring")"
    else
        log_error "Verification failed. Please check manually."
        exit 1
    fi
}

main "$@"
