#!/usr/bin/env bash
set -Eeuo pipefail

# Build seluruh dependensi native untuk Android ARM64.
# Dijalankan oleh GitHub Actions pada Ubuntu + Android NDK.

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME belum ditentukan}"

ROOT="${GITHUB_WORKSPACE:-$(pwd)}"
WORK="$ROOT/build-android-deps"
SRC="$WORK/src"
BUILD="$WORK/build"
PREFIX="$WORK/install"

API=24
ABI=arm64-v8a
JOBS="${BUILD_JOBS:-2}"

TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
CMAKE_TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"

for path in "$TOOLCHAIN/bin/clang" "$CMAKE_TOOLCHAIN"; do
    if [[ ! -e "$path" ]]; then
        echo "ERROR: NDK tidak lengkap: $path" >&2
        exit 1
    fi
done

export PATH="$TOOLCHAIN/bin:$PATH"
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
export ANDROID_API="$API"

CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
STRIP="$TOOLCHAIN/bin/llvm-strip"

export CC CXX AR RANLIB STRIP

mkdir -p "$SRC" "$BUILD" "$PREFIX"

echo "========== KONFIGURASI =========="
echo "NDK     : $ANDROID_NDK_HOME"
echo "ABI     : $ABI"
echo "API     : $API"
echo "PREFIX  : $PREFIX"
echo "JOBS    : $JOBS"

fetch_tag() {
    local name="$1"
    local url="$2"
    local tag="$3"

    if [[ ! -d "$SRC/$name/.git" ]]; then
        git clone --depth 1 --branch "$tag" "$url" "$SRC/$name"
    fi

    git -C "$SRC/$name" checkout --detach "$tag"
}

fetch_commit() {
    local name="$1"
    local url="$2"
    local commit="$3"

    if [[ ! -d "$SRC/$name/.git" ]]; then
        git clone --filter=blob:none "$url" "$SRC/$name"
    fi

    git -C "$SRC/$name" fetch --depth 1 origin "$commit"
    git -C "$SRC/$name" checkout --detach "$commit"
}

cmake_build() {
    local name="$1"
    local source="$2"
    shift 2

    cmake -S "$source" -B "$BUILD/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX;$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
        "$@"

    cmake --build "$BUILD/$name" --parallel "$JOBS"
    cmake --install "$BUILD/$name"
}

echo "========== 1. ZLIB =========="
fetch_tag zlib https://github.com/madler/zlib.git v1.3.1

cmake_build zlib "$SRC/zlib" \
    -DZLIB_BUILD_EXAMPLES=OFF

echo "========== 2. BZIP2 =========="
fetch_commit bzip2 https://sourceware.org/git/bzip2.git \
    f5dfc96063fef2d9c5446d0733867d899ef9b942

(
    cd "$SRC/bzip2"
    make -j"$JOBS" CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="-O2 -fPIC" LDFLAGS= libbz2.a
    mkdir -p "$PREFIX/include" "$PREFIX/lib"
    install -m 644 bzlib.h "$PREFIX/include/bzlib.h"
    install -m 644 libbz2.a "$PREFIX/lib/libbz2.a"
)

echo "========== 3. LZ4 =========="
fetch_commit lz4 https://github.com/lz4/lz4.git \
    707de205aac1f161be4cddb23d364e45bb566650

cmake_build lz4 "$SRC/lz4/build/cmake" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_LIBS=ON \
    -DLZ4_BUILD_CLI=OFF \
    -DLZ4_BUILD_LEGACY_LZ4C=OFF \
    -DLZ4_POSITION_INDEPENDENT_LIB=ON

echo "========== 4. XZ / LIBLZMA =========="
fetch_commit xz https://github.com/tukaani-project/xz.git \
    c308e9f27201300fd042d3e7324a21e55c25852f

cmake_build xz "$SRC/xz" \
    -DBUILD_SHARED_LIBS=OFF \
    -DXZ_NLS=OFF \
    -DXZ_DOC=OFF \
    -DBUILD_TESTING=OFF

echo "========== 5. ZSTD =========="
fetch_commit zstd https://github.com/facebook/zstd.git \
    439044eeb3394184e36bba7a6b60498c3bc7f9dc

cmake_build zstd "$SRC/zstd/build/cmake" \
    -DZSTD_BUILD_STATIC=ON \
    -DZSTD_BUILD_SHARED=OFF \
    -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_TESTS=OFF \
    -DZSTD_BUILD_CONTRIB=OFF

echo "========== 6. OPENSSL =========="
fetch_tag openssl https://github.com/openssl/openssl.git openssl-3.5.4

(
    cd "$SRC/openssl"

    ./Configure android-arm64 \
        -D__ANDROID_API__="$API" \
        no-shared \
        no-tests \
        --prefix="$PREFIX" \
        --openssldir="$PREFIX/ssl"

    make -j"$JOBS"
    make install_sw
)

echo "========== 7. LIBSSH2 =========="
fetch_tag libssh2 https://github.com/libssh2/libssh2.git libssh2-1.11.1

cmake_build libssh2 "$SRC/libssh2" \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCRYPTO_BACKEND=OpenSSL \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DZLIB_ROOT="$PREFIX" \
    -DZLIB_INCLUDE_DIR="$PREFIX/include" \
    -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
    -DENABLE_ZLIB_COMPRESSION=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF

echo "========== 8. CURL =========="
fetch_tag curl https://github.com/curl/curl.git curl-8_23_0

cmake_build curl "$SRC/curl" \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF \
    -DCURL_USE_OPENSSL=ON \
    -DCURL_USE_LIBSSH2=ON \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_ZLIB=ON \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
    -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.a" \
    -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DZLIB_ROOT="$PREFIX" \
    -DZLIB_INCLUDE_DIR="$PREFIX/include" \
    -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
    -DLIBSSH2_INCLUDE_DIR="$PREFIX/include" \
    -DLIBSSH2_LIBRARY="$PREFIX/lib/libssh2.a" \
    -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON

echo "========== 9. LIBARCHIVE =========="
fetch_tag libarchive https://github.com/libarchive/libarchive.git v3.8.1

cmake_build libarchive "$SRC/libarchive" \
    -DENABLE_TEST=OFF \
    -DENABLE_TAR=OFF \
    -DENABLE_CPIO=OFF \
    -DENABLE_CAT=OFF \
    -DENABLE_EXPAT=OFF \
    -DENABLE_LIBXML2=OFF \
    -DENABLE_ICONV=OFF \
    -DENABLE_ACL=OFF \
    -DENABLE_XATTR=OFF \
    -DENABLE_OPENSSL=ON \
    -DENABLE_ZLIB=ON \
    -DENABLE_BZip2=ON \
    -DENABLE_LZ4=ON \
    -DENABLE_LZMA=ON \
    -DENABLE_ZSTD=ON \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DZLIB_ROOT="$PREFIX" \
    -DBZIP2_INCLUDE_DIR="$PREFIX/include" \
    -DBZIP2_LIBRARY_RELEASE="$PREFIX/lib/libbz2.a" \
    -DZLIB_INCLUDE_DIR="$PREFIX/include" \
    -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
    -DLIBLZMA_INCLUDE_DIR="$PREFIX/include" \
    -DLIBLZMA_LIBRARY="$PREFIX/lib/liblzma.a" \
    -DLZ4_INCLUDE_DIR="$PREFIX/include" \
    -DLZ4_LIBRARY="$PREFIX/lib/liblz4.a" \
    -DZSTD_INCLUDE_DIR="$PREFIX/include" \
    -DZSTD_LIBRARY="$PREFIX/lib/libzstd.a" \
    -DOPENSSL_ROOT_DIR="$PREFIX" \
    -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE

echo "========== VERIFIKASI LIBRARY =========="
for lib in \
    libz.a libbz2.a liblz4.a liblzma.a libzstd.a \
    libssl.a libcrypto.a libssh2.a libcurl.a libarchive.a
do
    if [[ ! -f "$PREFIX/lib/$lib" ]]; then
        echo "ERROR: Library tidak ditemukan: $PREFIX/lib/$lib" >&2
        exit 1
    fi

    echo "[OK] $lib"
done

echo "========== SELESAI =========="
echo "ANDROID_DEPS=$PREFIX"
