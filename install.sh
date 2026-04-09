#!/bin/bash
# ============================================================================
# slim2UPnP installer
#
# Detects architecture, distribution and init system (systemd / OpenRC).
# Downloads the appropriate precompiled static binary from GitHub Releases
# if no local binary is available, then installs and sets up the service.
#
# Usage:
#   sudo ./install.sh              # Auto-download + install
#   sudo ./install.sh /path/to/bin # Install a specific binary
#   sudo ./install.sh --build      # Build from source + install
#   sudo ./install.sh --uninstall  # Remove slim2upnp
# ============================================================================

set -e

BINARY_NAME="slim2upnp"
INSTALL_DIR="/usr/local/bin"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"
GITHUB_REPO="cometdom/slim2UPnP"

# Colors (disabled if not a terminal)
if [ -t 1 ]; then
    GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; CYAN='\033[0;36m'; NC='\033[0m'
else
    GREEN=''; YELLOW=''; RED=''; CYAN=''; NC=''
fi

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[x]${NC} $*"; }
step()  { echo -e "${CYAN}[>]${NC} $*"; }

# ============================================================================
# Detection
# ============================================================================

detect_arch() {
    ARCH_RAW="$(uname -m)"
    case "$ARCH_RAW" in
        x86_64|amd64)
            # Check all x86-64-v3 required flags (not just AVX2)
            # x86-64-v3 requires: AVX, AVX2, FMA, BMI1, BMI2, LZCNT, MOVBE, XSAVE
            local v3_flags="avx2 fma bmi1 bmi2"
            local has_all=true
            for flag in $v3_flags; do
                if ! grep -qw "$flag" /proc/cpuinfo 2>/dev/null; then
                    has_all=false
                    break
                fi
            done
            if [ "$has_all" = true ]; then
                ARCH_VARIANT="x64-v3"
                ARCH_DESC="x86_64 AVX2"
            else
                ARCH_VARIANT="x64-v2"
                ARCH_DESC="x86_64 baseline"
            fi
            ;;
        aarch64|arm64)
            ARCH_VARIANT="aarch64"
            ARCH_DESC="ARM64"
            ;;
        *)
            ARCH_VARIANT=""
            ARCH_DESC="$ARCH_RAW (unsupported — build from source)"
            ;;
    esac
}

detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO_ID="${ID}"
        DISTRO_NAME="${PRETTY_NAME:-${NAME}}"
    elif [ -f /etc/gentoo-release ]; then
        DISTRO_ID="gentoo"
        DISTRO_NAME="Gentoo"
    else
        DISTRO_ID="unknown"
        DISTRO_NAME="Unknown"
    fi

    # Detect GentooPlayer (Gentoo-based with specific customizations)
    if [ -f /etc/gentooplayer-release ] || [ -d /opt/gentooplayer ]; then
        DISTRO_ID="gentooplayer"
        DISTRO_NAME="GentooPlayer"
    fi

    # Detect AudioLinux
    if [ -f /etc/audiolinux-release ] || grep -qi "audiolinux" /etc/os-release 2>/dev/null; then
        DISTRO_ID="audiolinux"
        DISTRO_NAME="AudioLinux"
    fi
}

detect_init_system() {
    if command -v systemctl >/dev/null 2>&1 && pidof systemd >/dev/null 2>&1; then
        INIT_SYSTEM="systemd"
    elif command -v rc-service >/dev/null 2>&1 || [ -f /sbin/openrc-run ]; then
        INIT_SYSTEM="openrc"
    elif [ -d /etc/init.d ]; then
        INIT_SYSTEM="sysvinit"
    else
        INIT_SYSTEM="unknown"
    fi
}

# ============================================================================
# Download from GitHub Releases
# ============================================================================

download_binary() {
    local variant="$1"
    local binary_file="slim2upnp-${variant}"
    local tmp_dir="/tmp/slim2upnp-install"

    # Need curl or wget
    local dl_cmd=""
    if command -v curl >/dev/null 2>&1; then
        dl_cmd="curl"
    elif command -v wget >/dev/null 2>&1; then
        dl_cmd="wget"
    else
        error "Neither curl nor wget found. Install one of them first."
        exit 1
    fi

    # Get latest release tag from GitHub API
    step "Fetching latest release from GitHub..."
    local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    local release_json=""

    if [ "$dl_cmd" = "curl" ]; then
        release_json="$(curl -sL "$api_url")"
    else
        release_json="$(wget -qO- "$api_url")"
    fi

    if [ -z "$release_json" ]; then
        error "Failed to fetch release info from GitHub"
        error "Check your internet connection"
        return 1
    fi

    # Extract tag name and download URL
    local tag_name=""
    local download_url=""
    local checksum_url=""

    # Parse JSON with grep/sed (no jq dependency)
    tag_name="$(echo "$release_json" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')"
    download_url="$(echo "$release_json" | grep "browser_download_url.*${binary_file}\"" | head -1 | sed 's/.*"browser_download_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')"
    checksum_url="$(echo "$release_json" | grep 'browser_download_url.*SHA256SUMS"' | head -1 | sed 's/.*"browser_download_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')"

    if [ -z "$tag_name" ]; then
        error "No release found on GitHub"
        error "The project may not have published any releases yet."
        error "Build from source instead: sudo ./install.sh --build"
        return 1
    fi

    if [ -z "$download_url" ]; then
        error "No binary found for architecture: $variant (release $tag_name)"
        error "Available binaries can be found at:"
        error "  https://github.com/${GITHUB_REPO}/releases/tag/${tag_name}"
        error "Build from source instead: sudo ./install.sh --build"
        return 1
    fi

    info "Latest release: $tag_name"
    step "Downloading $binary_file..."

    mkdir -p "$tmp_dir"

    if [ "$dl_cmd" = "curl" ]; then
        curl -sL -o "$tmp_dir/$binary_file" "$download_url"
    else
        wget -q -O "$tmp_dir/$binary_file" "$download_url"
    fi

    if [ ! -f "$tmp_dir/$binary_file" ] || [ ! -s "$tmp_dir/$binary_file" ]; then
        error "Download failed"
        rm -rf "$tmp_dir"
        return 1
    fi

    # Verify checksum if available
    if [ -n "$checksum_url" ]; then
        step "Verifying SHA256 checksum..."
        local checksums=""
        if [ "$dl_cmd" = "curl" ]; then
            checksums="$(curl -sL "$checksum_url")"
        else
            checksums="$(wget -qO- "$checksum_url")"
        fi

        local expected_hash="$(echo "$checksums" | grep "$binary_file" | awk '{print $1}')"
        if [ -n "$expected_hash" ]; then
            local actual_hash="$(sha256sum "$tmp_dir/$binary_file" | awk '{print $1}')"
            if [ "$expected_hash" = "$actual_hash" ]; then
                info "Checksum OK"
            else
                error "Checksum mismatch!"
                error "  Expected: $expected_hash"
                error "  Got:      $actual_hash"
                rm -rf "$tmp_dir"
                exit 1
            fi
        else
            warn "Checksum not found for $binary_file, skipping verification"
        fi
    fi

    chmod +x "$tmp_dir/$binary_file"

    # Install runtime deps if needed (static binaries skip this)
    install_runtime_deps "$tmp_dir/$binary_file"

    # Verify the binary runs — test beyond just --version to catch SIGILL in libs
    step "Verifying binary..."
    local verify_ok=false
    if "$tmp_dir/$binary_file" --version >/dev/null 2>&1; then
        # Also do a quick UPnP init test to catch SIGILL in statically linked libs
        # --list-renderers initializes libupnp and exits — triggers any illegal instructions
        if timeout 5 "$tmp_dir/$binary_file" --list-renderers >/dev/null 2>&1; then
            verify_ok=true
        else
            local exit_code=$?
            # exit code 132 = SIGILL (128 + signal 4)
            # exit code 124 = timeout (OK — means it ran but took time, no crash)
            if [ "$exit_code" = "124" ]; then
                verify_ok=true
            else
                warn "Binary crashed during UPnP init (exit code $exit_code)"
            fi
        fi
    fi
    if [ "$verify_ok" = true ]; then
        info "Binary verified OK"
    else
        # If v3 failed, try falling back to v2 (Illegal Instruction on some CPUs)
        if [ "$ARCH_VARIANT" = "x64-v3" ]; then
            warn "AVX2 binary failed (possible Illegal Instruction), trying baseline x64-v2..."
            ARCH_VARIANT="x64-v2"
            ARCH_DESC="x86_64 baseline (fallback)"
            local fallback_file="slim2upnp-x64-v2"
            local fallback_url="https://github.com/$GITHUB_REPO/releases/download/$latest_tag/$fallback_file"
            step "Downloading $fallback_file..."
            if curl -fSL -o "$tmp_dir/$fallback_file" "$fallback_url" 2>/dev/null; then
                chmod +x "$tmp_dir/$fallback_file"
                if "$tmp_dir/$fallback_file" --version >/dev/null 2>&1; then
                    info "Fallback binary x64-v2 verified OK"
                    binary_file="$fallback_file"
                else
                    error "Fallback binary also failed"
                    error "Build from source instead: sudo ./install.sh --build"
                    rm -rf "$tmp_dir"
                    return 1
                fi
            else
                error "Failed to download fallback binary"
                rm -rf "$tmp_dir"
                return 1
            fi
        else
            # Show which libraries are missing
            error "Downloaded binary failed to execute"
            if command -v ldd >/dev/null 2>&1; then
                local missing="$(ldd "$tmp_dir/$binary_file" 2>&1 | grep "not found")"
                if [ -n "$missing" ]; then
                    error "Missing libraries:"
                    echo "$missing" | while read -r line; do echo "    $line"; done
                fi
            fi
            error "Install missing libraries, or build from source: sudo ./install.sh --build"
            rm -rf "$tmp_dir"
            return 1
        fi
    fi

    BINARY_SRC="$tmp_dir/$binary_file"
    CLEANUP_TMP="$tmp_dir"
    return 0
}

# ============================================================================
# Build from source
# ============================================================================

build_from_source() {
    step "Building from source..."

    # Check for cmake (passthrough mode: only libupnp needed)
    if ! command -v cmake >/dev/null 2>&1; then
        error "CMake not found. Install build dependencies first:"
        case "$DISTRO_ID" in
            ubuntu|debian)
                echo "  sudo apt install build-essential cmake libupnp-dev"
                ;;
            fedora)
                echo "  sudo dnf install gcc-c++ cmake libupnp-devel"
                ;;
            arch|manjaro)
                echo "  sudo pacman -S base-devel cmake libupnp"
                ;;
            gentoo|gentooplayer)
                echo "  sudo emerge cmake net-libs/libupnp"
                ;;
            *)
                echo "  Install: cmake, g++, libupnp-dev"
                ;;
        esac
        exit 1
    fi

    cmake -B "$SCRIPT_DIR/build" -DCMAKE_BUILD_TYPE=Release "$SCRIPT_DIR"
    cmake --build "$SCRIPT_DIR/build" -j"$(nproc)"

    BINARY_SRC="$SCRIPT_DIR/build/$BINARY_NAME"

    if [ ! -f "$BINARY_SRC" ]; then
        error "Build failed"
        exit 1
    fi

    info "Build successful"
}

# ============================================================================
# Install
# ============================================================================

install_runtime_deps() {
    step "Checking runtime dependencies..."

    # Only install if the binary is dynamically linked
    if file "$1" 2>/dev/null | grep -q "statically linked"; then
        info "Static binary — no runtime dependencies needed"
        return 0
    fi

    # Passthrough mode: only libupnp is required (no decoder libraries)
    case "$DISTRO_ID" in
        ubuntu|debian)
            step "Installing runtime libraries..."
            apt-get update -qq
            apt-get install -y -qq libupnp17 2>/dev/null || \
                warn "libupnp could not be installed — check apt output above"
            ;;
        fedora)
            step "Installing runtime libraries..."
            dnf install -y libupnp || \
                warn "Failed to install libupnp"
            ;;
        arch|manjaro)
            step "Installing runtime libraries..."
            pacman -S --noconfirm --needed libupnp || \
                warn "libupnp could not be installed — check pacman output above"
            ;;
        gentoo|gentooplayer)
            info "Gentoo: ensure net-libs/libupnp is installed"
            ;;
        *)
            info "Check that libupnp is installed"
            ;;
    esac
}

install_binary() {
    local src="$1"

    if [ ! -f "$src" ]; then
        error "Binary not found: $src"
        exit 1
    fi

    install_runtime_deps "$src"

    # Stop running services before overwriting the binary
    if [ "$INIT_SYSTEM" = "systemd" ]; then
        systemctl stop slim2upnp 2>/dev/null || true
        systemctl stop slim2upnp-webui 2>/dev/null || true
    elif [ "$INIT_SYSTEM" = "openrc" ]; then
        rc-service slim2upnp stop 2>/dev/null || true
        rc-service slim2upnp-webui stop 2>/dev/null || true
    fi

    info "Installing $BINARY_NAME to $INSTALL_DIR/"
    install -m 755 "$src" "$INSTALL_DIR/$BINARY_NAME"

    # Install wrapper script
    if [ -f "$DIST_DIR/start-slim2upnp.sh" ]; then
        install -m 755 "$DIST_DIR/start-slim2upnp.sh" "$INSTALL_DIR/start-slim2upnp.sh"
        info "Wrapper script: $INSTALL_DIR/start-slim2upnp.sh"
    fi

    info "Installed: $INSTALL_DIR/$BINARY_NAME"
}

install_systemd() {
    local service_file="$DIST_DIR/slim2upnp.service"
    local config_file="$DIST_DIR/slim2upnp.default"

    info "Setting up systemd service..."

    # Service file
    if [ -f "$service_file" ]; then
        cp "$service_file" /etc/systemd/system/slim2upnp.service
        info "Service file: /etc/systemd/system/slim2upnp.service"
    else
        error "Service file not found: $service_file"
        return 1
    fi

    # Configuration: merge existing values with new template
    if [ -f /etc/default/slim2upnp ]; then
        # Save user values from existing config
        local saved_renderer="" saved_renderer_url="" saved_lms="" saved_player=""
        local saved_no_dsd="" saved_interface="" saved_http_port="" saved_verbose=""
        . /etc/default/slim2upnp 2>/dev/null || true
        saved_renderer="$RENDERER"
        saved_renderer_url="$RENDERER_URL"
        saved_lms="$LMS_SERVER"
        saved_player="$PLAYER_NAME"
        saved_no_dsd="$NO_DSD"
        saved_interface="$INTERFACE"
        saved_http_port="$HTTP_PORT"
        saved_verbose="$VERBOSE"

        # Install new template
        cp "$config_file" /etc/default/slim2upnp

        # Re-apply saved values (only non-empty ones)
        [ -n "$saved_renderer" ] && sed -i "s|^RENDERER=.*|RENDERER=\"$saved_renderer\"|" /etc/default/slim2upnp
        [ -n "$saved_renderer_url" ] && sed -i "s|^RENDERER_URL=.*|RENDERER_URL=\"$saved_renderer_url\"|" /etc/default/slim2upnp
        [ -n "$saved_lms" ] && sed -i "s|^LMS_SERVER=.*|LMS_SERVER=\"$saved_lms\"|" /etc/default/slim2upnp
        [ -n "$saved_player" ] && sed -i "s|^PLAYER_NAME=.*|PLAYER_NAME=\"$saved_player\"|" /etc/default/slim2upnp
        [ -n "$saved_no_dsd" ] && sed -i "s|^NO_DSD=.*|NO_DSD=\"$saved_no_dsd\"|" /etc/default/slim2upnp
        [ -n "$saved_interface" ] && sed -i "s|^INTERFACE=.*|INTERFACE=\"$saved_interface\"|" /etc/default/slim2upnp
        [ -n "$saved_http_port" ] && sed -i "s|^HTTP_PORT=.*|HTTP_PORT=\"$saved_http_port\"|" /etc/default/slim2upnp
        [ -n "$saved_verbose" ] && sed -i "s|^VERBOSE=.*|VERBOSE=\"$saved_verbose\"|" /etc/default/slim2upnp

        info "Config updated: /etc/default/slim2upnp (your settings preserved)"
    else
        cp "$config_file" /etc/default/slim2upnp
        info "Config file: /etc/default/slim2upnp"
    fi

    systemctl daemon-reload
    info "systemd reloaded"

    # Install WebUI
    install_webui

    echo ""
    info "Next steps:"
    echo "  1. Open the WebUI: http://$(hostname -I | awk '{print $1}'):8082"
    echo "     Or edit /etc/default/slim2upnp manually"
    echo "  2. Set your renderer name and LMS server"
    echo "  3. sudo systemctl start slim2upnp"
    echo "  4. sudo systemctl enable slim2upnp   # auto-start at boot"
    echo ""
    echo "  Useful commands:"
    echo "    sudo systemctl status slim2upnp     # check status"
    echo "    sudo journalctl -u slim2upnp -f     # follow logs"
    echo "    sudo systemctl restart slim2upnp    # restart after config change"
}

install_openrc() {
    local init_file="$DIST_DIR/slim2upnp.openrc"
    local config_file="$DIST_DIR/slim2upnp.default"

    info "Setting up OpenRC service..."

    # Init script
    if [ -f "$init_file" ]; then
        cp "$init_file" /etc/init.d/slim2upnp
        chmod 755 /etc/init.d/slim2upnp
        info "Init script: /etc/init.d/slim2upnp"
    else
        error "Init script not found: $init_file"
        return 1
    fi

    # Configuration (OpenRC uses /etc/conf.d/): merge existing values
    if [ -f /etc/conf.d/slim2upnp ]; then
        local saved_renderer="" saved_renderer_url="" saved_lms="" saved_player=""
        local saved_no_dsd="" saved_interface="" saved_http_port="" saved_verbose=""
        . /etc/conf.d/slim2upnp 2>/dev/null || true
        saved_renderer="$RENDERER"
        saved_renderer_url="$RENDERER_URL"
        saved_lms="$LMS_SERVER"
        saved_player="$PLAYER_NAME"
        saved_no_dsd="$NO_DSD"
        saved_interface="$INTERFACE"
        saved_http_port="$HTTP_PORT"
        saved_verbose="$VERBOSE"

        cp "$config_file" /etc/conf.d/slim2upnp

        [ -n "$saved_renderer" ] && sed -i "s|^RENDERER=.*|RENDERER=\"$saved_renderer\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_renderer_url" ] && sed -i "s|^RENDERER_URL=.*|RENDERER_URL=\"$saved_renderer_url\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_lms" ] && sed -i "s|^LMS_SERVER=.*|LMS_SERVER=\"$saved_lms\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_player" ] && sed -i "s|^PLAYER_NAME=.*|PLAYER_NAME=\"$saved_player\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_no_dsd" ] && sed -i "s|^NO_DSD=.*|NO_DSD=\"$saved_no_dsd\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_interface" ] && sed -i "s|^INTERFACE=.*|INTERFACE=\"$saved_interface\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_http_port" ] && sed -i "s|^HTTP_PORT=.*|HTTP_PORT=\"$saved_http_port\"|" /etc/conf.d/slim2upnp
        [ -n "$saved_verbose" ] && sed -i "s|^VERBOSE=.*|VERBOSE=\"$saved_verbose\"|" /etc/conf.d/slim2upnp

        info "Config updated: /etc/conf.d/slim2upnp (your settings preserved)"
    else
        cp "$config_file" /etc/conf.d/slim2upnp
        info "Config file: /etc/conf.d/slim2upnp"
    fi

    # Install WebUI
    install_webui

    echo ""
    info "Next steps:"
    echo "  1. Open the WebUI: http://$(hostname -I | awk '{print $1}'):8082"
    echo "     Or edit /etc/conf.d/slim2upnp manually"
    echo "  2. Set your renderer name and LMS server"
    echo "  3. sudo rc-service slim2upnp start"
    echo "  4. sudo rc-update add slim2upnp default   # auto-start at boot"
    echo ""
    echo "  Useful commands:"
    echo "    sudo rc-service slim2upnp status     # check status"
    echo "    tail -f /var/log/slim2upnp.log       # follow logs"
    echo "    sudo rc-service slim2upnp restart    # restart after config change"
}

install_webui() {
    local webui_src="$SCRIPT_DIR/webui"
    local webui_dest="/opt/slim2upnp-webui"

    if [ ! -f "$webui_src/diretta_webui.py" ]; then
        warn "WebUI files not found in $webui_src, skipping"
        return 1
    fi

    # Check Python3
    if ! command -v python3 >/dev/null 2>&1; then
        warn "Python 3 not found — WebUI requires Python 3"
        warn "Install it with your package manager, then re-run install.sh"
        return 1
    fi

    step "Installing WebUI to $webui_dest..."
    mkdir -p "$webui_dest"
    cp "$webui_src/diretta_webui.py" "$webui_dest/"
    cp "$webui_src/config_parser.py" "$webui_dest/"
    cp -r "$webui_src/profiles" "$webui_dest/"
    cp -r "$webui_src/templates" "$webui_dest/"
    cp -r "$webui_src/static" "$webui_dest/"

    # Install webUI service
    if [ "$INIT_SYSTEM" = "systemd" ]; then
        cp "$webui_src/slim2upnp-webui.service" /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable slim2upnp-webui 2>/dev/null || true
        systemctl start slim2upnp-webui 2>/dev/null || true
        info "WebUI service installed (systemd)"
    elif [ "$INIT_SYSTEM" = "openrc" ]; then
        cp "$webui_src/slim2upnp-webui.openrc" /etc/init.d/slim2upnp-webui
        chmod 755 /etc/init.d/slim2upnp-webui
        rc-update add slim2upnp-webui default 2>/dev/null || true
        rc-service slim2upnp-webui start 2>/dev/null || true
        info "WebUI service installed (OpenRC)"
    fi

    info "WebUI available at: http://$(hostname -I | awk '{print $1}'):8082"
}

install_no_service() {
    warn "Init system not detected (or unsupported)."
    echo ""
    info "The binary is installed. Run manually:"
    echo "  $INSTALL_DIR/$BINARY_NAME -r \"My Renderer\" -s <LMS_IP>"
    echo ""
    echo "  Or create your own service/startup script."
}

# ============================================================================
# Uninstall
# ============================================================================

do_uninstall() {
    info "Uninstalling slim2UPnP..."

    # Stop services if running
    if [ "$INIT_SYSTEM" = "systemd" ]; then
        systemctl stop slim2upnp 2>/dev/null || true
        systemctl stop slim2upnp-webui 2>/dev/null || true
        systemctl disable slim2upnp 2>/dev/null || true
        systemctl disable slim2upnp-webui 2>/dev/null || true
        rm -f /etc/systemd/system/slim2upnp.service
        rm -f /etc/systemd/system/slim2upnp-webui.service
        systemctl daemon-reload
        info "systemd services removed"
    elif [ "$INIT_SYSTEM" = "openrc" ]; then
        rc-service slim2upnp stop 2>/dev/null || true
        rc-service slim2upnp-webui stop 2>/dev/null || true
        rc-update del slim2upnp default 2>/dev/null || true
        rc-update del slim2upnp-webui default 2>/dev/null || true
        rm -f /etc/init.d/slim2upnp
        rm -f /etc/init.d/slim2upnp-webui
        info "OpenRC services removed"
    fi

    # Remove binary, wrapper, and webUI
    rm -f "$INSTALL_DIR/$BINARY_NAME"
    rm -f "$INSTALL_DIR/start-slim2upnp.sh"
    rm -rf /opt/slim2upnp-webui
    info "Binary and WebUI removed"

    # Keep config files (user may want them)
    if [ -f /etc/default/slim2upnp ]; then
        warn "Config kept: /etc/default/slim2upnp (remove manually if desired)"
    fi
    if [ -f /etc/conf.d/slim2upnp ]; then
        warn "Config kept: /etc/conf.d/slim2upnp (remove manually if desired)"
    fi

    info "Uninstall complete."
}

# ============================================================================
# Main
# ============================================================================

# Must be root
if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root (sudo ./install.sh)"
    exit 1
fi

detect_arch
detect_distro
detect_init_system

echo "═══════════════════════════════════════════════════════"
echo "  slim2UPnP installer"
echo "═══════════════════════════════════════════════════════"
echo ""
info "Architecture: $ARCH_DESC ($ARCH_RAW)"
info "Distribution: $DISTRO_NAME ($DISTRO_ID)"
info "Init system:  $INIT_SYSTEM"
echo ""

BINARY_SRC=""
CLEANUP_TMP=""

# Handle arguments
case "${1:-}" in
    --uninstall)
        do_uninstall
        exit 0
        ;;
    --build)
        build_from_source
        ;;
    "")
        # No argument: try local build first, then download
        if [ -f "$SCRIPT_DIR/build/$BINARY_NAME" ]; then
            # Check if local build is up to date with source
            build_ver=""
            build_ver="$("$SCRIPT_DIR/build/$BINARY_NAME" --version 2>&1 | grep -oP 'v\K[0-9]+\.[0-9]+\.[0-9]+-?[a-z]*' | head -1)" || true
            src_ver=""
            src_ver="$(grep -oP 'SLIM2UPNP_VERSION\s+"\K[^"]+' "$SCRIPT_DIR/src/main.cpp" 2>/dev/null)" || true

            if [ -n "$build_ver" ] && [ -n "$src_ver" ] && [ "$build_ver" != "$src_ver" ]; then
                warn "Local build is v$build_ver but source is v$src_ver"
                step "Rebuilding from source..."
                build_from_source
            else
                info "Found local build (v${build_ver:-unknown})"
                BINARY_SRC="$SCRIPT_DIR/build/$BINARY_NAME"
            fi
        elif [ -n "$ARCH_VARIANT" ]; then
            info "No local build found, downloading precompiled binary..."
            echo ""
            if ! download_binary "$ARCH_VARIANT"; then
                echo ""
                error "Download failed. You can:"
                echo "  1. Build from source:  sudo ./install.sh --build"
                echo "  2. Specify a binary:   sudo ./install.sh /path/to/slim2upnp"
                exit 1
            fi
        else
            error "No precompiled binary available for $ARCH_RAW"
            error "Build from source: sudo ./install.sh --build"
            exit 1
        fi
        ;;
    *)
        # Explicit binary path
        if [ -f "$1" ]; then
            BINARY_SRC="$1"
        else
            error "File not found: $1"
            echo ""
            echo "Usage:"
            echo "  sudo ./install.sh              # Auto-download or use local build"
            echo "  sudo ./install.sh --build      # Build from source and install"
            echo "  sudo ./install.sh /path/to/bin # Install a specific binary"
            echo "  sudo ./install.sh --uninstall  # Remove slim2upnp"
            exit 1
        fi
        ;;
esac

echo ""

# Install binary
install_binary "$BINARY_SRC"

# Cleanup temp download
if [ -n "$CLEANUP_TMP" ]; then
    rm -rf "$CLEANUP_TMP"
fi

# Install service based on init system
case "$INIT_SYSTEM" in
    systemd)
        install_systemd
        ;;
    openrc)
        install_openrc
        ;;
    *)
        install_no_service
        ;;
esac

LOCAL_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -z "$LOCAL_IP" ] && LOCAL_IP="<your-machine-ip>"

echo ""
echo "═══════════════════════════════════════════════════════"
info "Installation complete!"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "  Open the Web Configuration UI in your browser:"
echo ""
echo -e "    ${CYAN:-}http://${LOCAL_IP}:8082${NC:-}"
echo ""
echo "  From there you can configure your renderer, LMS server,"
echo "  and start playing — no command line needed."
echo ""
echo "  Then select 'slim2UPnP' as the player in LMS and press play."
echo ""
echo "═══════════════════════════════════════════════════════"
echo ""
