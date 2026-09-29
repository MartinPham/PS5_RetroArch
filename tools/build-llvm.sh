#!/usr/bin/env bash
# Build LLVM for the PS5 as static archives, for RPCS3's PPU and SPU
# recompilers (docs/RPCS3_PORT.md).
#
# The source is my fork of llvm-project, PS5_LLVM (github.com/mihawk-99/PS5_LLVM),
# whose main is llvmorg-22.1.8 -- the revision RPCS3 pins as its
# 3rdparty/llvm/llvm submodule -- plus the changes the console's ABI needs
# (SmallVector's alignment). It is fetched sparsely and built once, outside
# RPCS3's tree, so a change to RPCS3 never rebuilds LLVM:
# X86 only, no tools, tests, docs or examples, no zlib, zstd, libxml2 or
# network, no crash-signal handlers of LLVM's own (the title reports crashes),
# with ccache. The code generators LLVM's build runs (llvm-tblgen and
# llvm-min-tblgen) are built for the host first, from the same tree, so the
# console build defines no tablegen programs of its own.
#
# Output: .deps/native/llvm-ps5 (lib/*.a, include/, lib/cmake/llvm), with the
# revision in .deps/native/llvm-ps5/.revision.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

revision=98b45cc22e98844b7d49bbedefdfee04a740650a  # PS5_LLVM main: llvmorg-22.1.8 and the port
prefix="$root/.deps/native/llvm-ps5"
[[ -f $prefix/.revision && $(<"$prefix/.revision") == "$revision" ]] && {
    echo "==> [llvm] $revision already built in $prefix"; exit 0; }

sdk="$root/.deps/native/ps5-payload-sdk"
[[ -x $sdk/bin/prospero-clang ]] || { echo "error: bootstrap this project's SDK first" >&2; exit 2; }
export PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=${PS5_CLANG:-/usr/bin/clang}

launcher=()
command -v ccache >/dev/null && launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
source_dir="$root/.deps/llvm-project"
if [[ ! -d $source_dir/.git ]]; then
    echo "==> [llvm] fetching $revision (sparse: llvm, cmake, third-party)"
    rm -rf -- "$source_dir"
    git init --quiet "$source_dir"
    git -C "$source_dir" remote add origin https://github.com/mihawk-99/PS5_LLVM.git
    git -C "$source_dir" config remote.origin.promisor true
    git -C "$source_dir" config remote.origin.partialclonefilter blob:none
    git -C "$source_dir" sparse-checkout set --no-cone /llvm/ /cmake/ /third-party/
fi
# The sibling ../PS5_LLVM is itself a sparse partial clone, so the fetch is from
# the published fork; a change there is pushed before it is built here.
git -C "$source_dir" remote set-url origin https://github.com/mihawk-99/PS5_LLVM.git
git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null ||
    git -C "$source_dir" fetch --quiet --depth 1 --filter=blob:none origin "$revision"
git -C "$source_dir" checkout --force --quiet "$revision"

# The host's tablegen programs, with the host's compilers.
host_build="$root/build/llvm-host-tblgen"
echo "==> [llvm] host tablegen"
cmake -S "$source_dir/llvm" -B "$host_build" -G Ninja "${launcher[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cc -DCMAKE_CXX_COMPILER=c++ \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    > "$host_build.configure.log" 2>&1 || { tail -30 "$host_build.configure.log" >&2; exit 1; }
ninja -C "$host_build" -j "${JOBS:-16}" llvm-tblgen llvm-min-tblgen > "$host_build.log"

build="$root/build/llvm-ps5"
# Empty libm, librt, libpthread, libdl and libutil: configure's checks link
# them, and their functions are the console's own libc and libkernel.
empty_libs="$build-empty-libs"
mkdir -p "$empty_libs"
for lib in m rt pthread dl util; do
    [[ -f $empty_libs/lib$lib.a ]] || "$sdk/bin/prospero-ar" rc "$empty_libs/lib$lib.a"
done
echo "==> [llvm] configuring"
cmake -S "$source_dir/llvm" -B "$build" -G Ninja "${launcher[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$root/tooling/rpcs3/ps5-toolchain.cmake" -DPS5_EMPTY_LIBS="$empty_libs" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_HOST_TRIPLE=x86_64-unknown-freebsd14 -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-freebsd14 \
    -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_ENABLE_PIC=ON \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF \
    -DLLVM_ENABLE_HTTPLIB=OFF -DLLVM_ENABLE_BACKTRACES=OFF -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
    -DLLVM_ENABLE_THREADS=ON -DLLVM_ENABLE_WARNINGS=OFF \
    -DLLVM_NATIVE_TOOL_DIR="$host_build/bin" \
    > "$build.configure.log" 2>&1 || { tail -40 "$build.configure.log" >&2; exit 1; }
# The libraries only: the tablegen programs stay defined for the console too,
# and would link as console programs with a C++ runtime the core never links.
echo "==> [llvm] building the libraries"
mapfile -t libraries < <(ninja -C "$build" -t targets all | sed -n 's/^\(lib\/libLLVM[^:]*\.a\): .*/\1/p')
ninja -C "$build" -j "${JOBS:-16}" "${libraries[@]}"
cmake --install "$build" > "$build.install.log"
printf '%s\n' "$revision" > "$prefix/.revision"
printf '==> [llvm] %s: %s of archives in %s\n' "$revision" "$(du -sh "$prefix/lib" | cut -f1)" "$prefix"
