#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Read a literal path before Python is available. Never source/eval local data.
# build.py validates the complete profile before configuring anything.
source_dir="$script_dir/src"
next_source=0
help_only=0
for argument in "$@"; do
    if [ "$next_source" = 1 ]; then
        source_dir=$argument
        next_source=0
        continue
    fi
    case "$argument" in
        --source) next_source=1 ;;
        --source=*) source_dir=${argument#--source=} ;;
        --help|-h) help_only=1 ;;
    esac
done
resolved_source=$(CDPATH= cd -- "$source_dir" 2>/dev/null && pwd) || resolved_source=''
if [ -z "${VINC_BUILD_INTERPRETER:-}" ] && [ "$resolved_source" = "$script_dir/src" ] && [ "$help_only" = 0 ]; then
    system=$(uname -s)
    case "$system" in
        Linux)
            system=linux
            if [ -n "${WSL_DISTRO_NAME:-}" ]; then
                system=wsl-$(printf '%s' "$WSL_DISTRO_NAME" | LC_ALL=C tr -c 'A-Za-z0-9._-' '_')
            fi
            ;;
        Darwin) system=macos ;;
    esac
    arch=$(uname -m)
    case "$arch" in aarch64) arch=arm64 ;; esac
    profile="$script_dir/.vinc/toolchains/$system-$arch.conf"
    if [ -e "$profile" ]; then
        if [ "$(wc -c < "$profile")" -gt 16384 ]; then
            printf '%s\n' "Toolchain profile too large: $profile" >&2
            exit 1
        fi
        saved_python=''
        format=''
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                format=*) format=${line#format=} ;;
                build_python=*)
                    if [ -n "$saved_python" ]; then
                        printf '%s\n' "Duplicate Python in $profile" >&2
                        exit 1
                    fi
                    saved_python=${line#build_python=}
                    ;;
            esac
        done < "$profile"
        case "$saved_python" in
            /*) ;;
            *)
                printf '%s\n' "Invalid Python path in $profile; rerun setup." >&2
                exit 1
                ;;
        esac
        if [ "$format" != 1 ] || [ ! -x "$saved_python" ]; then
            printf '%s\n' "Remembered Python unavailable or invalid profile: $profile; rerun setup." >&2
            exit 1
        fi
        exec "$saved_python" -B "$script_dir/build.py" "$@"
    fi
fi
if [ -n "${VINC_BUILD_INTERPRETER:-}" ]; then
    if ! command -v "$VINC_BUILD_INTERPRETER" >/dev/null 2>&1; then
        printf 'Build failed: Python was not found: %s. Run bash tools/setup.sh or correct VINC_BUILD_INTERPRETER.\n' "$VINC_BUILD_INTERPRETER" >&2
        exit 1
    fi
    exec "$VINC_BUILD_INTERPRETER" -B "$script_dir/build.py" "$@"
fi
if command -v python3 >/dev/null 2>&1; then
    exec python3 -B "$script_dir/build.py" "$@"
fi
printf '%s\n' 'Build failed: Python was not found. Run bash tools/setup.sh or set VINC_BUILD_INTERPRETER.' >&2
exit 1
