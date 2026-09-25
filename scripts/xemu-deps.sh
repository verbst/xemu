#!/usr/bin/env bash
#
# Report, and optionally install, the packages xemu needs to build.
#
# Checking is the default. Installing requires --install, so running this on a
# machine that builds other projects tells you what would change before
# anything does.
#
# On MSYS2 this never runs "pacman -Syu". A full system upgrade would move
# shared packages such as gcc and glib2 underneath every other project in the
# same environment, which is rarely what someone setting up one project wants.
# Packages are installed with --needed so anything already present is left at
# the version it has.

set -uo pipefail

usage() {
    cat <<EOF
Usage: ${0##*/} [--install] [--snapshot FILE]

  --install        Install missing packages. Without it, they are only listed.
  --snapshot FILE  Write the installed versions of every package this script
                   manages to FILE. Written automatically before --install so
                   there is a record to compare against or roll back to.
  -h, --help       Show this message.
EOF
}

do_install=0
snapshot=""

while [ $# -gt 0 ]; do
    case "$1" in
        --install)  do_install=1 ;;
        --snapshot) shift; snapshot="${1:-}" ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------------------------------------------------------------- environment

case "$(uname -s)" in
    Linux)
        if command -v apt-get >/dev/null 2>&1; then
            env_name="Debian/Ubuntu"
            pkg_mgr="apt"
        else
            echo "Only apt-based Linux distributions are handled here." >&2
            echo "Install the equivalents of the Debian packages in debian/control." >&2
            exit 1
        fi
        ;;
    MINGW64_NT*|MINGW32_NT*|UCRT64_NT*|CLANG64_NT*|MSYS_NT*)
        env_name="MSYS2 ${MSYSTEM:-unknown}"
        pkg_mgr="pacman"
        case "${MSYSTEM:-}" in
            MINGW64) pkg_prefix="mingw-w64-x86_64" ;;
            UCRT64)  pkg_prefix="mingw-w64-ucrt-x86_64" ;;
            CLANG64) pkg_prefix="mingw-w64-clang-x86_64" ;;
            *)
                echo "Run this from a MINGW64, UCRT64 or CLANG64 shell." >&2
                echo "MSYSTEM is currently '${MSYSTEM:-unset}'." >&2
                exit 1
                ;;
        esac
        ;;
    *)
        echo "Unsupported platform: $(uname -s)" >&2
        exit 1
        ;;
esac

# ------------------------------------------------------------------- packages

if [ "$pkg_mgr" = "apt" ]; then
    packages=(
        # build tools
        meson ninja-build pkg-config cmake git
        # the build system's Python helpers
        python3-pip python3-venv python3-tomli python3-yaml python3-setuptools
        # QEMU core
        libglib2.0-dev libpixman-1-dev zlib1g-dev libssl-dev
        # xemu
        libepoxy-dev libsamplerate0-dev libpcap-dev libslirp-dev
        libvulkan-dev libusb-1.0-0-dev libcurl4-gnutls-dev
        libpipewire-0.3-dev libgtk-3-dev
        # SDL3 is built from a subproject. Its CMake fails rather than
        # degrades when a sub-feature of an enabled backend is missing, so the
        # X11 set has to be complete.
        libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev
        libxfixes-dev libxss-dev libxkbcommon-dev
        libwayland-dev wayland-protocols libdecor-0-dev
        libdrm-dev libgbm-dev libudev-dev libdbus-1-dev libibus-1.0-dev
        libasound2-dev libpulse-dev
    )
else
    # Unprefixed MSYS2 packages, installed once per installation rather than
    # once per environment.
    packages_msys=(git)
    #
    # Named individually rather than through the toolchain and vulkan-devel
    # groups. Asking for a group makes pacman stop and ask which members are
    # wanted, which is no good unattended, and accepting the default would pull
    # the whole toolchain -- including a compiler upgrade -- into an
    # environment that may be shared with other projects. The packages below
    # are the members actually needed.
    #
    # SDL3 is deliberately absent: the build fetches and builds it as a
    # subproject, and no MSYS2 package of that name exists.
    packages=(
        gcc meson ninja cmake pkgconf python make
        # The settings header is generated from a YAML description, and the
        # generator runs from the build's virtual environment -- which is
        # created with access to system site packages, so installing it here
        # is enough for the build to find it.
        python-yaml
        glib2 pixman zlib openssl
        libepoxy libsamplerate libslirp libusb curl
        vulkan-headers vulkan-loader
        nlohmann-json
    )
fi

# --------------------------------------------------------------------- checks

is_installed() {
    if [ "$pkg_mgr" = "apt" ]; then
        dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "ok installed"
        return
    fi

    pacman -Qi "$1" >/dev/null 2>&1 && return 0

    # A group is never "installed" in its own right, so ask instead whether
    # every member is. Without this a fully populated toolchain reports as
    # missing and the script offers to install what is already there.
    local members
    members="$(pacman -Sg "$1" 2>/dev/null | awk '{print $2}')"
    if [ -n "$members" ]; then
        local m
        for m in $members; do
            pacman -Qi "$m" >/dev/null 2>&1 || return 1
        done
        return 0
    fi

    return 1
}

installed_version() {
    if [ "$pkg_mgr" = "apt" ]; then
        dpkg-query -W -f='${Version}' "$1" 2>/dev/null
    else
        pacman -Qi "$1" 2>/dev/null | awk -F': +' '/^Version/ {print $2; exit}'
    fi
}

# Expand to fully qualified names so the rest of the script can treat both
# package managers the same way.
qualified=()
if [ "$pkg_mgr" = "apt" ]; then
    qualified=("${packages[@]}")
else
    for p in "${packages_msys[@]}"; do qualified+=("$p"); done
    for p in "${packages[@]}";      do qualified+=("${pkg_prefix}-${p}"); done
fi

echo "Environment: ${env_name}"
echo

missing=()
for p in "${qualified[@]}"; do
    if is_installed "$p"; then
        printf '  present  %-42s %s\n' "$p" "$(installed_version "$p")"
    else
        printf '  MISSING  %s\n' "$p"
        missing+=("$p")
    fi
done

write_snapshot() {
    local out="$1"
    {
        echo "# xemu build dependencies on ${env_name}"
        echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        for p in "${qualified[@]}"; do
            v="$(installed_version "$p")"
            [ -n "$v" ] && echo "${p}=${v}"
        done
    } > "$out"
    echo "Wrote package snapshot to ${out}"
}

[ -n "$snapshot" ] && write_snapshot "$snapshot"

echo
if [ ${#missing[@]} -eq 0 ]; then
    echo "All dependencies are present. Build with ./build.sh"
    exit 0
fi

echo "${#missing[@]} package(s) missing."

# A name the package manager does not recognise would abort the whole install,
# taking the packages that were fine down with it. Report those separately and
# carry on with the rest.
if [ "$pkg_mgr" = "pacman" ]; then
    known=()
    unknown=()
    for p in "${missing[@]}"; do
        if pacman -Si "$p" >/dev/null 2>&1 || pacman -Sg "$p" >/dev/null 2>&1; then
            known+=("$p")
        else
            unknown+=("$p")
        fi
    done
    if [ ${#unknown[@]} -gt 0 ]; then
        echo
        echo "Not available in this environment's repositories:"
        printf '  %s\n' "${unknown[@]}"
        echo "Continuing without them; the build may still succeed if they are"
        echo "provided another way."
        echo
    fi
    missing=("${known[@]}")
    if [ ${#missing[@]} -eq 0 ]; then
        echo "Nothing left to install."
        exit 0
    fi
fi

if [ "$pkg_mgr" = "apt" ]; then
    install_cmd=(sudo apt-get install -y "${missing[@]}")
else
    install_cmd=(pacman -S --needed "${missing[@]}")
fi

if [ "$do_install" -eq 0 ]; then
    echo "Install them with:"
    echo
    echo "    ${install_cmd[*]}"
    echo
    echo "Or re-run this script with --install."
    exit 1
fi

# A record of what was installed before the change, so a regression in another
# project built in the same environment can be traced or reverted.
if [ -z "$snapshot" ]; then
    # Kept outside the source tree so a checkout is never polluted by it.
    snapshot_dir="${XDG_CACHE_HOME:-${HOME}/.cache}/xemu"
    mkdir -p "$snapshot_dir"
    snapshot="${snapshot_dir}/deps-$(date -u +%Y%m%dT%H%M%SZ).txt"
    write_snapshot "$snapshot"
fi

if [ "$pkg_mgr" = "pacman" ]; then
    echo
    echo "Note: installing into a shared MSYS2 environment. Packages already"
    echo "present are left alone, but pacman may pull newer versions of shared"
    echo "dependencies. ${snapshot} records the current versions."
    echo
fi

echo "Running: ${install_cmd[*]}"
"${install_cmd[@]}"
