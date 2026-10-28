#!/usr/bin/env bash
# Prepare dependencies only. Never configure or build the caller's project.
set -euo pipefail

setup_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository="$(cd -- "$setup_dir/.." && pwd)"
check_only=0
with_ui=1
with_python=1
temporary=""
compiler=""
c_compiler=""
python=""
system=""
ubuntu_version=""
apt_updated=0

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'HELP'
Usage: bash tools/setup.sh [--check] [--no-ui] [--no-python]
Install/check dependencies, restore pinned submodules, and run npm ci.
No CMake configure, project build, tests, or browser downloads are performed.
--check       Check only; no installs, downloads, or dependency restoration.
--no-ui       Skip Node/npm and frontend packages.
--no-python   Skip Python SDK interpreter/development requirements.
CC/CXX select a compiler executable; VINC_SETUP_PYTHON selects an interpreter.
HELP
}

parse_options() {
    for option in "$@"; do
        case "$option" in
            --check) check_only=1 ;;
            --no-ui) with_ui=0 ;;
            --no-python) with_python=0 ;;
            --help|-h)
                usage
                return 2
                ;;
            *) fail "Unknown option: $option (use --help)" ;;
        esac
    done
}

detect_system() {
    case "$(uname -s)" in
        Linux)
            [[ -r /etc/os-release ]] || fail "Cannot identify this Linux distribution"
            # os-release is maintained by the system, not by repository inputs.
            . /etc/os-release
            [[ ${ID:-} == ubuntu ]] || fail "Automatic Linux setup currently supports Ubuntu 22.04/24.04 only"
            case "${VERSION_ID:-}" in
                22.04|24.04) ubuntu_version="$VERSION_ID" ;;
                *) fail "Unsupported Ubuntu release: ${VERSION_ID:-unknown}" ;;
            esac
            system=ubuntu
            ;;
        Darwin) system=macos ;;
        *) fail "Use tools/setup.bat on Windows; supported shells run on Ubuntu or macOS" ;;
    esac
    case "$(uname -m)" in
        x86_64|arm64|aarch64) ;;
        *) fail "Only 64-bit x86_64/arm64 hosts are handled by setup" ;;
    esac
}

as_root() {
    if [[ $(id -u) == 0 ]]; then
        "$@"
    else
        command -v sudo >/dev/null || fail "sudo is required to install system packages"
        sudo "$@"
    fi
}

install_packages() {
    (( $# > 0 )) || return 0
    (( check_only == 0 )) || fail "Missing/incompatible dependencies: $*; rerun without --check"
    if [[ $system == ubuntu ]]; then
        if (( apt_updated == 0 )); then
            as_root apt-get update
            apt_updated=1
        fi
        as_root apt-get install -y --no-install-recommends "$@"
    else
        command -v brew >/dev/null || fail "Install Homebrew from https://brew.sh and rerun setup"
        [[ $(id -u) != 0 ]] || fail "Run Homebrew setup as your normal user"
        brew install "$@"
    fi
}

version_at_least() {
    local actual="${1%%[-+]*}"
    local minimum="$2"
    local actual_parts
    local minimum_parts
    local index
    IFS=. read -r -a actual_parts <<< "$actual"
    IFS=. read -r -a minimum_parts <<< "$minimum"
    for index in 0 1 2; do
        local left="${actual_parts[index]:-0}"
        local right="${minimum_parts[index]:-0}"
        [[ $left =~ ^[0-9]+$ && $right =~ ^[0-9]+$ ]] || return 1
        if (( 10#$left > 10#$right )); then
            return 0
        fi
        if (( 10#$left < 10#$right )); then
            return 1
        fi
    done
}

tool_version_ok() {
    local tool="$1"
    local minimum="$2"
    command -v "$tool" >/dev/null || return 1
    local output
    output="$("$tool" --version 2>/dev/null)" || return 1
    [[ $output =~ ([0-9]+\.[0-9]+\.[0-9]+) ]] || return 1
    version_at_least "${BASH_REMATCH[1]}" "$minimum"
}

prepare_tools() {
    local packages=()
    command -v git >/dev/null || packages+=(git)
    tool_version_ok cmake 3.20.0 || packages+=(cmake)
    command -v ninja >/dev/null || packages+=(ninja-build)
    if [[ $system == macos ]]; then
        if ! xcrun --find clang++ >/dev/null 2>&1; then
            if (( check_only == 0 )); then
                xcode-select --install || true
            fi
            fail "Complete Apple's Command Line Tools installation, then rerun setup"
        fi
        # Homebrew's formula is ninja, whereas Ubuntu's package is ninja-build.
        # Apple ships Bash 3.2: do not expand an empty array under nounset.
        if [[ -n ${packages[*]-} ]]; then
            local index
            for index in "${!packages[@]}"; do
                [[ ${packages[index]} != ninja-build ]] || packages[index]=ninja
            done
        fi
    fi
    if [[ -n ${packages[*]-} ]]; then
        install_packages "${packages[@]}"
    fi
    tool_version_ok cmake 3.20.0 || fail "CMake 3.20+ is not available on PATH after installation"
    git --version
    cmake --version | head -1
    ninja --version
}

probe_compiler() {
    "$1" -std=c++23 -pthread "$setup_dir/setup/toolchain.cpp" \
        -o "$temporary/toolchain" >"$temporary/compiler.log" 2>&1
}

prepare_compilers() {
    if [[ -n ${CXX:-} ]]; then
        compiler="$CXX"
        probe_compiler "$compiler" || {
            cat "$temporary/compiler.log" >&2
            fail "Selected CXX cannot compile/link the C++23 probe; choose one compiler executable"
        }
    elif [[ $system == ubuntu ]]; then
        local candidate
        for candidate in g++ g++-13 g++-12; do
            if command -v "$candidate" >/dev/null && probe_compiler "$candidate"; then
                compiler="$(command -v "$candidate")"
                break
            fi
        done
        if [[ -z $compiler ]]; then
            local gcc_major=13
            [[ $ubuntu_version != 22.04 ]] || gcc_major=12
            install_packages "gcc-$gcc_major" "g++-$gcc_major"
            compiler="$(command -v "g++-$gcc_major")"
            probe_compiler "$compiler" || {
                cat "$temporary/compiler.log" >&2
                fail "Installed compiler/standard library did not pass the C++23 probe"
            }
        fi
    else
        compiler="$(xcrun --find clang++)"
        probe_compiler "$compiler" || {
            cat "$temporary/compiler.log" >&2
            fail "Update Xcode/Command Line Tools or select a compatible compiler with CXX"
        }
    fi

    c_compiler="${CC:-}"
    if [[ -z $c_compiler ]]; then
        case "$(basename "$compiler")" in
            g++*) c_compiler="${compiler/g++/gcc}" ;;
            clang++*) c_compiler="${compiler/clang++/clang}" ;;
            *) c_compiler=cc ;;
        esac
    fi
    command -v "$c_compiler" >/dev/null || fail "C compiler missing: $c_compiler (set CC)"
    command -v make >/dev/null || install_packages make
    if ! "$c_compiler" "$setup_dir/setup/zlib.c" -lz -o "$temporary/zlib" >"$temporary/zlib.log" 2>&1; then
        if [[ $system == ubuntu ]]; then
            install_packages zlib1g-dev
        else
            cat "$temporary/zlib.log" >&2
            fail "Selected C compiler cannot link SDK zlib; check Xcode/SDK/CC selection"
        fi
        "$c_compiler" "$setup_dir/setup/zlib.c" -lz -o "$temporary/zlib" || fail "zlib development files remain unavailable"
    fi
    "$compiler" --version | head -1
    printf 'C++23/64-bit/atomic/threads probe: PASS\nCC=%s\nCXX=%s\n' "$c_compiler" "$compiler"
}

prepare_python() {
    (( with_python == 1 )) || return 0
    python="${VINC_SETUP_PYTHON:-python3}"
    if ! command -v "$python" >/dev/null || ! "$python" "$setup_dir/setup/check_python.py"; then
        [[ -z ${VINC_SETUP_PYTHON:-} ]] || fail "Explicit VINC_SETUP_PYTHON is incompatible; install matching development files"
        if [[ $system == ubuntu ]]; then
            install_packages python3 python3-dev
        else
            install_packages python@3.13
            python="$(brew --prefix python@3.13)/bin/python3.13"
        fi
        "$python" "$setup_dir/setup/check_python.py" || fail "Python SDK dependency check failed"
    fi
    python="$(command -v "$python")"
}

node_ok() {
    command -v node >/dev/null || return 1
    node -e 'const [a,b]=process.versions.node.split(".").map(Number); process.exit((a===20&&b>=19)||(a===22&&b>=12)||a>22 ? 0 : 1)'
}

node_tools_ok() {
    node_ok && command -v npm >/dev/null
}

prepare_node() {
    (( with_ui == 1 )) || return 0
    # Prefer an existing compatible installation; a private Ubuntu install is
    # considered only when PATH's Node is absent or incompatible.
    if ! node_tools_ok && [[ $system == ubuntu ]]; then
        local private_node="${XDG_DATA_HOME:-$HOME/.local/share}/open-vinc/node/bin"
        if [[ -x $private_node/node ]]; then
            export PATH="$private_node:$PATH"
        fi
        if ! node_tools_ok; then
            (( check_only == 0 )) || fail "Node.js 20.19+ (20.x), 22.12+ or newer compatible release is missing"
            command -v curl >/dev/null || install_packages curl ca-certificates
            command -v xz >/dev/null || install_packages xz-utils
            local arch=x64
            [[ $(uname -m) == x86_64 ]] || arch=arm64
            local base=https://nodejs.org/dist/latest-v24.x
            curl --fail --show-error --location --proto '=https' --tlsv1.2 \
                "$base/SHASUMS256.txt" -o "$temporary/SHASUMS256.txt"
            local filename
            filename="$(awk -v suffix="-linux-$arch.tar.xz" '$2 ~ /^node-v24\.[0-9]+\.[0-9]+-linux-/ && substr($2,length($2)-length(suffix)+1)==suffix {print $2}' "$temporary/SHASUMS256.txt")"
            [[ $filename =~ ^node-v24\.[0-9]+\.[0-9]+-linux-(x64|arm64)\.tar\.xz$ ]] || fail "No unique Node 24 archive found in official checksums"
            local version="${filename%-linux-*}"
            curl --fail --show-error --location --proto '=https' --tlsv1.2 \
                "https://nodejs.org/dist/${version#node-}/$filename" -o "$temporary/$filename"
            (cd "$temporary" && awk -v file="$filename" '$2==file' SHASUMS256.txt | sha256sum --check --strict)
            local destination="${private_node%/bin}"
            [[ ! -e $destination ]] || fail "Private Node directory is incompatible; inspect $destination before replacing it"
            mkdir -p "$destination"
            tar -xJf "$temporary/$filename" --strip-components=1 -C "$destination"
            export PATH="$private_node:$PATH"
        fi
        printf 'For future shells, add to PATH: %s\n' "$private_node"
    elif ! node_tools_ok; then
        install_packages node@24
        export PATH="$(brew --prefix node@24)/bin:$PATH"
    fi
    node_ok || fail "Node version remains incompatible"
    command -v npm >/dev/null || fail "npm is missing from the selected Node installation"
    node --version
    npm --version
}

prepare_submodules() {
    git -C "$repository" rev-parse --is-inside-work-tree >/dev/null || fail "Use a Git checkout of Open-VINC, including .git metadata"
    local status
    status="$(git -C "$repository" submodule status --recursive)"
    [[ $status != *$'\nU'* && $status != U* ]] || fail "Resolve submodule merge conflicts before setup"
    # Refuse dirty tracked/untracked files before changing any initialized checkout.
    git -C "$repository" submodule foreach --quiet --recursive \
        'test -z "$(git status --porcelain --untracked-files=normal)" || { echo "Dirty submodule: $displaypath" >&2; exit 1; }' \
        || fail "Preserve submodule changes before setup; no forced checkout is used"
    if (( check_only == 1 )); then
        [[ $status != -* && $status != +* && $status != *$'\n-'* && $status != *$'\n+'* ]] \
            || fail "Submodules are missing or differ from pinned revisions"
    else
        git -C "$repository" submodule update --init --recursive
    fi
    printf 'Pinned submodules: PASS\n'
}

prepare_frontend() {
    (( with_ui == 1 )) || return 0
    if (( check_only == 1 )); then
        (cd "$repository/src/vinc/ui/web" && npm ls --include=dev --all >/dev/null) \
            || fail "Frontend dependencies are missing/incompatible; rerun setup without --check"
    else
        (cd "$repository/src/vinc/ui/web" && npm ci --include=dev --no-audit --no-fund)
    fi
    node "$setup_dir/setup/check_frontend.mjs" "$repository/src/vinc/ui/web"
}

main() {
    local parsed=0
    parse_options "$@" || parsed=$?
    [[ $parsed != 2 ]] || return 0
    detect_system
    temporary="$(mktemp -d "${TMPDIR:-/tmp}/open-vinc-setup.XXXXXXXX")"
    # Only this freshly created directory is owned by the script.
    trap 'rm -rf -- "$temporary"' EXIT
    printf 'Open-VINC dependency setup (%s; check-only=%s)\n' "$system" "$check_only"
    prepare_tools
    prepare_compilers
    prepare_python
    prepare_node
    prepare_submodules
    prepare_frontend
    printf '\nDependency checks passed. No project was configured or built.\n'
    printf 'Select the checked CC/CXX in your own build environment.\n'
    if [[ -n $python ]]; then
        printf 'For Python SDK builds, select Python_EXECUTABLE=%s\n' "$python"
    fi
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
