#!/usr/bin/env bash
# Build a self-contained Linux helper. Do not silently publish a dynamically
# linked binary: its loader or glibc version may be absent in another distro.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
compiler="${CXX:-g++}"
build_dir="$project_dir/build/helper"
output_dir="$project_dir/dist"

if ! command -v "$compiler" >/dev/null 2>&1; then
    printf 'C++ compiler not found: %s. Set CXX to an installed Linux compiler.\n' "$compiler" >&2
    exit 1
fi
case "$(uname -m)" in
    x86_64|aarch64) ;;
    *) printf 'Unsupported Linux architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac
mkdir -p -- "$build_dir" "$output_dir"
objects=()
for module in main procfs network services runtime_stacks; do
    object="$build_dir/$module.o"
    "$compiler" -std=c++17 -pthread -O2 -Wall -Wextra -Wpedantic \
        -I"$project_dir/vendor" -c "$project_dir/helper/$module.cpp" -o "$object"
    objects+=("$object")
done
if ! "$compiler" -static -pthread "${objects[@]}" -o "$build_dir/wsl-observer"; then
    printf 'Static helper link failed. Static C/C++ runtime libraries are required; no dynamic fallback was published.\n' >&2
    exit 1
fi
# Keep relocatable objects in the package so the static observer can be relinked
# against compatible modified runtime libraries without rebuilding its source.
mkdir -p "$output_dir/relink" "$output_dir/licenses"
install -m 0644 "${objects[@]}" "$output_dir/relink/"
printf '%s\n' 'Relink on x86-64 Linux with: g++ -static -pthread main.o procfs.o network.o services.o runtime_stacks.o -o wsl-observer' \
    'A compatible modified libc/libstdc++ can be selected using your compiler sysroot and library search flags.' \
    'Observer source and the normal build recipe are included in the project repository.' > "$output_dir/relink/README.txt"
for notice in /usr/share/doc/libc6/copyright /usr/share/doc/gcc-*/copyright; do
    if [[ -f "$notice" ]]; then
        package_name="$(basename "$(dirname "$notice")")"
        install -m 0644 "$notice" "$output_dir/licenses/$package_name.copyright.txt"
    fi
done
install -m 0755 -- "$build_dir/wsl-observer" "$output_dir/wsl-observer"
printf 'Built %s\n' "$output_dir/wsl-observer"
