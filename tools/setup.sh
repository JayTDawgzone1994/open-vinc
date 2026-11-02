#!/usr/bin/env bash
# Prepare dependencies only. Never configure or build the caller's project.
set -euo pipefail

setup_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository="$(cd -- "$setup_dir/.." && pwd)"
check_only=0
recommended=0
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
Usage: bash tools/setup.sh [--recommended] [--check] [--no-ui] [--no-python]
Install/check dependencies, restore pinned submodules, and run npm ci.
No CMake configure, project build, tests, or browser downloads are performed.
--check       Check only; no installs, downloads, or dependency restoration.
--recommended Prefer the documented installation candidates; keep compatible tools.
--no-ui       Skip Node/npm and frontend packages.
--no-python   Skip Python Module development files; still check the build interpreter.
CC/CXX select a compiler executable; VINC_SETUP_PYTHON selects an interpreter.
HELP
}

parse_options() {
    for option in "$@"; do
        case "$option" in
            --check) check_only=1 ;;
            --recommended) recommended=1 ;;
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
            system=linux
            if [[ -r /etc/os-release ]]; then
                # Distribution metadata selects installation recipes, not build eligibility.
                . /etc/os-release
                if [[ ${ID:-} == ubuntu ]]; then
                    system=ubuntu
                    ubuntu_version="${VERSION_ID:-unknown}"
                fi
            fi
            ;;
        Darwin) system=macos ;;
        *) fail "Use tools/setup.bat on Windows; this shell checks Linux or macOS" ;;
    esac
    case "$(uname -m)" in
        x86_64|arm64|aarch64) ;;
        *) fail "Only 64-bit x86_64/arm64 hosts are handled by setup" ;;
    esac
}

require_install_recipe() {
    if [[ $system == macos ]]; then
        return 0
    fi
    if [[ $system == ubuntu && ( $ubuntu_version == 22.04 || $ubuntu_version == 24.04 ) ]]; then
        return 0
    fi
    fail "No automatic installation recipe for $system ${ubuntu_version:-}. Install the missing dependencies manually and rerun --check; existing compatible tools can still build."
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
    require_install_recipe
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
    if (( recommended == 1 )) && [[ $system == ubuntu ]]; then
        prepare_recommended_tools
    fi
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

prepare_recommended_tools() {
    # Private tool directories do not replace distribution tools or PATH
    # globally. Publish a directory only after download, checksum and extraction.
    local tool
    local arch
    local base
    local archive
    local checksum
    local destination
    local root="${XDG_DATA_HOME:-$HOME/.local/share}/open-vinc"
    arch="$(uname -m)"
    [[ $arch != arm64 ]] || arch=aarch64
    for tool in cmake ninja; do
        if [[ $tool == cmake ]] && tool_version_ok cmake 3.20.0; then
            continue
        fi
        if [[ $tool == ninja ]] && command -v ninja >/dev/null; then
            continue
        fi
        destination="$root/$tool-recommended"
        if [[ -x $destination/bin/$tool ]]; then
            export PATH="$destination/bin:$PATH"
            continue
        fi
        (( check_only == 0 )) || fail "Missing $tool; rerun setup --recommended without --check"
        require_install_recipe
        command -v curl >/dev/null || install_packages curl ca-certificates
        [[ ! -e $destination ]] || fail "Inspect incomplete tool directory before replacing it: $destination"
        mkdir -p "$temporary/$tool/bin" "$root"
        if [[ $tool == cmake ]]; then
            base=https://github.com/Kitware/CMake/releases/download/v4.4.5
            archive="cmake-4.4.5-linux-$arch.tar.gz"
            curl --fail --show-error --location --proto '=https' --tlsv1.2 \
                "$base/cmake-4.4.5-SHA-256.txt" -o "$temporary/cmake-checksums"
            checksum="$(awk -v file="$archive" '$2==file {print $1}' "$temporary/cmake-checksums")"
        else
            command -v unzip >/dev/null || install_packages unzip
            base=https://github.com/ninja-build/ninja/releases/download/v1.13.2
            archive=ninja-linux.zip
            checksum=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
            if [[ $arch == aarch64 ]]; then
                archive=ninja-linux-aarch64.zip
                checksum=fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc
            fi
        fi
        [[ $checksum =~ ^[0-9a-fA-F]{64}$ ]] || fail "No unique official checksum for $archive"
        curl --fail --show-error --location --proto '=https' --tlsv1.2 "$base/$archive" -o "$temporary/$archive"
        (cd "$temporary" && printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --strict)
        if [[ $tool == cmake ]]; then
            tar -xzf "$temporary/$archive" --strip-components=1 -C "$temporary/$tool"
        else
            unzip -q "$temporary/$archive" ninja -d "$temporary/$tool/bin"
            chmod +x "$temporary/$tool/bin/ninja"
        fi
        "$temporary/$tool/bin/$tool" --version
        mv "$temporary/$tool" "$destination"
        export PATH="$destination/bin:$PATH"
    done
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
    elif [[ $system != macos ]]; then
        local candidate
        for candidate in c++ g++ clang++ g++-13 g++-12; do
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
        if [[ $system != macos ]]; then
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
    local python_mode=--with-development
    (( with_python == 1 )) || python_mode=--interpreter-only
    python="${VINC_SETUP_PYTHON:-python3}"
    if ! command -v "$python" >/dev/null || ! "$python" "$setup_dir/setup/check_python.py" "$python_mode"; then
        [[ -z ${VINC_SETUP_PYTHON:-} ]] || fail "Explicit VINC_SETUP_PYTHON is incompatible; install matching development files"
        if (( recommended == 1 )) && [[ $system != macos ]]; then
            if [[ $system != ubuntu || $ubuntu_version != 24.04 ]]; then
                fail "Recommended Python: CPython 3.12 with matching development files (minimum 3.11). Prepare it manually and set VINC_SETUP_PYTHON=/absolute/path/to/python3. No third-party repository or Python installer will be added."
            fi
            python=python3.12
            if (( with_python == 1 )); then
                install_packages python3.12 python3.12-dev
            else
                install_packages python3.12
            fi
        elif [[ $system != macos ]]; then
            if (( with_python == 1 )); then
                install_packages python3 python3-dev
            else
                install_packages python3
            fi
        else
            local series=3.13
            (( recommended == 0 )) || series=3.14
            install_packages "python@$series"
            python="$(brew --prefix "python@$series")/bin/python$series"
        fi
        "$python" "$setup_dir/setup/check_python.py" "$python_mode" || fail "System Python still does not meet requirements. Install/select a compatible interpreter with VINC_SETUP_PYTHON; system defaults were not replaced."
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
            require_install_recipe
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
        if [[ $system != macos ]]; then
            fail "Node/npm is missing or incompatible. Install compatible tools and rerun --check; no automatic Node recipe exists for this distribution."
        fi
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

save_toolchain() {
    (( check_only == 0 )) || return 0
    local arguments=(--build-python "$python" --config-python "$python"
        --cc "$(command -v "$c_compiler")" --cxx "$(command -v "$compiler")"
        --cmake "$(command -v cmake)" --ninja "$(command -v ninja)"
        --compiler-version "$("$compiler" --version | head -1)")
    if (( with_ui == 1 )); then
        arguments+=(--node "$(command -v node)" --npm "$(command -v npm)")
    fi
    if [[ $system == macos ]]; then
        arguments+=(--developer-dir "${DEVELOPER_DIR:-$(xcode-select -p)}")
    fi
    "$python" "$setup_dir/toolchain_profile.py" "${arguments[@]}"
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
    if (( recommended == 1 )); then
        printf 'Recommended mode: keep compatible tools; use platform installation candidates when needed.\n'
        printf 'Python: Ubuntu 24.04=3.12, macOS=3.14; other Linux: prepare 3.12 manually.\n'
    fi
    prepare_tools
    prepare_compilers
    prepare_python
    prepare_node
    prepare_submodules
    prepare_frontend
    save_toolchain
    printf '\nDependency checks passed. No project was configured or built.\n'
    printf 'Root build wrappers reuse saved selections for this checkout; direct CMake/external projects remain explicit.\n'
    if [[ -n $python ]]; then
        printf 'Build interpreter: %s\nFor direct CMake, select Python3_EXECUTABLE explicitly; root wrappers use the saved selection.\n' "$python"
        if (( with_python == 1 )); then
            printf 'For Python Module builds, choose Custom in App Kconfig and set CONFIG_VINC_PYTHON_EXECUTABLE to: %s\n' "$python"
        fi
    fi
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
