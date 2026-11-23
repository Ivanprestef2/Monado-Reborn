#!/usr/bin/env bash
# Minimal LGPL FFmpeg for Monado. The runtime only needs this much (runtime/hle/movie.c):
#   - libavformat: Open/read/seek WebM (Matroska) from disk
#   - libavcodec:  decode VP9 video and Vorbis audio
#   - libavutil:   frames, packets, timestamps
#   - libswscale:  NV12 -> RGBA for the texture output path
# No encoders, muxers, filters, network, devices, hardware acceleration or external codec libraries.
#
# The distro packages are full builds: MSYS2's FFmpeg pulls 99 DLLs (137 MB) into the Windows ZIP, and Ubuntu's
# build does the same to the AppImage. This builds only what the game uses.
#
#   scripts/build_ffmpeg.sh [--prefix /usr/local] [--jobs N] [--extra-decoders vp8,h264,aac]
#
# On Windows run it from an MSYS2 UCRT64 shell (needs gcc, make, nasm and zlib). CI installs to /ucrt64.
# Linux needs a C compiler, make, nasm, zlib1g-dev and curl; CI installs to /usr/local.
# For a local check use a scratch prefix (--prefix build/ffmpeg): installing over a distro prefix replaces its
# full FFmpeg libraries and breaks the ffmpeg CLI.
set -euo pipefail

version=9.0.2
sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
url=https://ffmpeg.org/releases/ffmpeg-$version.tar.xz

# The shipped cutscenes are all VP9 + Vorbis (14 WebM files). Keep the native fallbacks the movie HLE names
# (VP8, H.264, AAC) with `--extra-decoders vp8,h264,aac`; they cost about 2 MB and pull in no external libraries.
decoders=vp9,vorbis
prefix=/usr/local
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix=$2; shift 2;;
    --jobs) jobs=$2; shift 2;;
    --extra-decoders) decoders=$decoders,$2; shift 2;;
    -h|--help)
      cat <<'EOF'
usage: scripts/build_ffmpeg.sh [--prefix DIR] [--jobs N] [--extra-decoders LIST]

Builds a minimal LGPL FFmpeg (VP9 + Vorbis decode from WebM, file protocol,
swscale) for the runtime's movie player. Defaults: prefix /usr/local, jobs from
the host, decoders vp9,vorbis. --extra-decoders vp8,h264,aac keeps the native
fallbacks the movie HLE names (about 2 MB more, still no external libraries).
EOF
      exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) target=mingw32; threads=--enable-w32threads; cc=${CC:-gcc}; cxx=${CXX:-g++};;
  Linux*)               target=linux;   threads=--enable-pthreads;   cc=${CC:-cc};  cxx=${CXX:-c++};;
  *) echo "unsupported host: $(uname -s)" >&2; exit 2;;
esac

# Keep the build out of the source tree and out of the repository. MSYS2 and Linux both have mktemp.
build=$(mktemp -d "${TMPDIR:-/tmp}/monado-ffmpeg-$version.XXXXXX")
trap 'rm -rf "$build"' EXIT
tarball=$build/ffmpeg-$version.tar.xz

echo "FFmpeg $version: VP9 + Vorbis, Matroska, file protocol, swscale -> $prefix"
curl -fsSL --retry 3 -o "$tarball" "$url"
( cd "$build" && echo "$sha256  ffmpeg-$version.tar.xz" | sha256sum -c - )
tar -xf "$tarball" -C "$build"
src=$build/ffmpeg-$version

cd "$build"
"$src/configure" --prefix="$prefix" --target-os="$target" --arch=x86_64 --cc="$cc" --cxx="$cxx" \
  --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network \
  --disable-avdevice --disable-avfilter --disable-swresample --disable-gpl --disable-version3 \
  --disable-debug --enable-pic --enable-runtime-cpudetect \
  --enable-decoder="$decoders" --enable-demuxer=matroska --enable-protocol=file \
  --enable-swscale --enable-zlib --enable-shared --disable-static "$threads"
make -j"$jobs"
make install
echo "FFmpeg $version installed in $prefix (LGPL 2.1+, no GPL components)"
