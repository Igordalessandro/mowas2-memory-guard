#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "$0")" && pwd)
toolchain_root=${LLVM_MINGW_ROOT:-/opt/llvm-mingw}
bin_dir="$toolchain_root/bin"
cc="$bin_dir/i686-w64-mingw32-clang"
cxx="$bin_dir/i686-w64-mingw32-clang++"

if [[ ! -x "$cc" || ! -x "$cxx" ]]; then
    echo "Set LLVM_MINGW_ROOT to an llvm-mingw installation." >&2
    exit 1
fi

build_dir="$project_root/build-linux"
dist_dir="$project_root/dist"
mkdir -p "$build_dir" "$dist_dir"
rm -f "$build_dir"/*.o

common=(
    -O2 -D_WIN32_WINNT=0x0601
    -I"$project_root/src"
    -I"$project_root/thirdparty/minhook/include"
    -I"$project_root/thirdparty/minhook/src"
)

c_sources=(
    thirdparty/minhook/src/buffer.c
    thirdparty/minhook/src/hook.c
    thirdparty/minhook/src/trampoline.c
    thirdparty/minhook/src/hde/hde32.c
)

objects=()
for source in "${c_sources[@]}"; do
    object="$build_dir/${source//\//_}.o"
    "$cc" "${common[@]}" -c "$project_root/$source" -o "$object"
    objects+=("$object")
done

while IFS= read -r -d '' source; do
    relative=${source#"$project_root/"}
    object="$build_dir/${relative//\//_}.o"
    "$cxx" "${common[@]}" -std=c++17 -fms-extensions -c "$source" -o "$object"
    objects+=("$object")
done < <(find "$project_root/src" -name '*.cpp' -print0 | sort -z)

"$cxx" -shared -static -o "$dist_dir/dbghelp.dll" \
    "${objects[@]}" \
    -Wl,--subsystem,windows -Wl,--kill-at -Wl,--out-implib,"$build_dir/libdbghelp.a" \
    -lkernel32 -luser32 -ladvapi32 -lpsapi

"$bin_dir/i686-w64-mingw32-strip" "$dist_dir/dbghelp.dll"
"$bin_dir/i686-w64-mingw32-objdump" -p "$dist_dir/dbghelp.dll" | \
    sed -n '/Export Table/,+35p'
sha256sum "$dist_dir/dbghelp.dll"
