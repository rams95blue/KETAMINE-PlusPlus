#!/usr/bin/env bash
# Cross-builds winmm.dll on Linux with clang + MinGW-w64 (static: no runtime DLLs needed).
# Needs: clang, lld, mingw-w64 (posix threads), python3; ImGui v1.92.9 and MinHook v1.3.4 sources in $DEPS.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="${DEPS:-$ROOT/deps}"
OUT="$ROOT/build-mingw"
VERSION="$(grep -oP 'project\(KetaminePlusPlus VERSION \K[0-9.]+' "$ROOT/CMakeLists.txt")"
mkdir -p "$OUT/obj"

TARGET=x86_64-w64-mingw32
CC="clang --target=$TARGET"
CXX="clang++ --target=$TARGET"
GCCDIR="$(dirname "$(x86_64-w64-mingw32-g++-posix -print-libgcc-file-name)")"
COMMON="-O2 -fms-extensions -DNDEBUG -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 \
  -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS -I$DEPS/imgui -I$DEPS/imgui/backends -I$DEPS/minhook/include \
  -Wno-ignored-pragmas -Wno-unknown-pragmas -Wno-pragma-pack"
CXXFLAGS="$COMMON -std=c++20 -fexceptions -DKM_VERSION=\"$VERSION\""
# Use the posix-thread GCC runtime (std::mutex / std::thread).
CXXFLAGS="$CXXFLAGS -isystem /usr/lib/gcc/x86_64-w64-mingw32/$(basename "$GCCDIR")/include/c++ \
  -isystem /usr/lib/gcc/x86_64-w64-mingw32/$(basename "$GCCDIR")/include/c++/x86_64-w64-mingw32"

python3 "$ROOT/tools/check_tables.py" "$ROOT/src"
python3 "$ROOT/tools/gen_mingw_proxy.py" "$ROOT/src" "$OUT"

# Delay-loaded DLLs need LLVM-style import libraries (the GNU ones from MinGW can't be delay-loaded by lld).
mkdir -p "$OUT/implib"
printf 'LIBRARY d3dcompiler_47.dll\nEXPORTS\n    D3DCompile\n' > "$OUT/implib/d3dcompiler_47.def"
printf 'LIBRARY dxgi.dll\nEXPORTS\n    CreateDXGIFactory\n    CreateDXGIFactory1\n    CreateDXGIFactory2\n' > "$OUT/implib/dxgi.def"
for d in d3dcompiler_47 dxgi; do
  llvm-dlltool -m i386:x86-64 -d "$OUT/implib/$d.def" -l "$OUT/implib/lib$d.a"
done

objs=()
compile() { # source, compiler, flags
  local o="$OUT/obj/$(echo "$1" | sed 's#[/.]#_#g').o"
  $2 $3 -c "$1" -o "$o"
  objs+=("$o")
}
for f in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_win32.cpp \
         backends/imgui_impl_dx11.cpp backends/imgui_impl_dx12.cpp; do
  compile "$DEPS/imgui/$f" "$CXX" "$CXXFLAGS -w" &
done
for f in src/buffer.c src/hook.c src/trampoline.c src/hde/hde64.c; do
  compile "$DEPS/minhook/$f" "$CC" "$COMMON -w" &
done
wait
objs=("$OUT"/obj/*.o)
for f in dllmain overlay menu ue features esp lang fov scan fixes jump hooks minimap uitweaks; do
  compile "$ROOT/src/$f.cpp" "$CXX" "$CXXFLAGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch -Wno-unknown-warning-option"
done
$CC -c "$OUT/winmm_stubs.S" -o "$OUT/obj/winmm_stubs.o"; objs+=("$OUT/obj/winmm_stubs.o")
(cd "$ROOT/src" && x86_64-w64-mingw32-windres resources.rc -O coff -o "$OUT/obj/resources.o"); objs+=("$OUT/obj/resources.o")

$CXX -shared -fuse-ld=lld -static -static-libgcc -static-libstdc++ -s -o "$OUT/winmm.dll" "${objs[@]}" \
  -L"$OUT/implib" -Wl,--delayload=d3dcompiler_47.dll -Wl,--delayload=dxgi.dll -Wl,--exclude-all-symbols \
  -ld3d11 -ld3d12 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -luser32 -ladvapi32 -lshell32 -lole32 -ldelayimp \
  -L"$GCCDIR" -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic
echo "Built $OUT/winmm.dll ($VERSION)"
