#!/usr/bin/env bash
# The static LLVM that the shipped ounce-builder links (`--features static-llvm,clang`): LLVM, clang and lld of
# one pinned release, X86 only, the libraries alone (no tools, tests, docs, bindings, terminfo, libxml2, zlib or
# zstd), built for x86-64-v3 like the game. Then:
#   LLVM_STATIC_DIR=<dir>/build cargo build --release -p ounce-builder --features static-llvm,clang
#
#   scripts/linux/static-llvm.sh [<dir>]    default work/llvm-static: <dir>/src (a shallow clone of the tag), <dir>/build
#
# About 10 minutes on 16 threads, 4.4 GB at the peak with 12 jobs (STATIC_LLVM_JOBS), 3.9 GB on disk. Needs git,
# cmake, ninja, clang and lld.
set -euo pipefail
TAG=llvmorg-23.1.5
root=$(cd "$(dirname "$0")/../.." && pwd)
dir=${1:-$root/work/llvm-static}
jobs=${STATIC_LLVM_JOBS:-12}
mkdir -p "$dir"
if [ ! -d "$dir/src/.git" ]; then
  git clone -q --depth 1 --branch "$TAG" --single-branch https://github.com/llvm/llvm-project.git "$dir/src"
fi
have=$(git -C "$dir/src" describe --tags 2>/dev/null || echo none)
[ "$have" = "$TAG" ] || { echo "static-llvm: $dir/src is $have, not $TAG: remove it and run again"; exit 1; }
flags="-march=x86-64-v3 -ffunction-sections -fdata-sections"
cmake -G Ninja -S "$dir/src/llvm" -B "$dir/build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_USE_LINKER=lld \
  -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
  -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-pc-linux-gnu \
  -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DLLVM_BUILD_TOOLS=OFF -DCLANG_BUILD_TOOLS=OFF -DLLD_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DCLANG_BUILD_EXAMPLES=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_OBJC_REWRITER=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_PARALLEL_LINK_JOBS=2 > "$dir/configure.log"
# (the libraries the builder links and what they need, llvm-config for its flags, clang's resource headers)
ninja -C "$dir/build" -j "$jobs" llvm-config clang-resource-headers \
  clangCodeGen clangFrontend clangDriver clangSerialization clangParse clangSema clangAnalysis clangAST clangEdit \
  clangLex clangBasic clangAPINotes clangSupport clangOptions lldELF lldCommon \
  LLVMX86CodeGen LLVMX86AsmParser LLVMX86Desc LLVMX86Info LLVMPasses LLVMIRReader
echo "static-llvm: $dir/build ($("$dir/build/bin/llvm-config" --version))"
