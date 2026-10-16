#!/usr/bin/env bash
# Builds the Luling transcode pack: a full FFmpeg with encoders, used only for
# format conversion / resizing / compression.
#
#   ffmpeg.exe / ffprobe.exe, statically linked, no DLLs next to them.
#   Unlike the preview pack this one carries encoders, so it links libx264 and
#   libx265 and is therefore distributed under GPL-2.0-or-later. Luling invokes
#   it strictly as a separate process (never links it), and ships the source
#   offer below; the application itself stays under its own licence.
#
#   --enable-nonfree is still never passed, and there is no network support.
#
# Runs in MSYS2 MINGW64. Usage: packs/transcode/build.sh <ffmpeg-version> <out>
set -euo pipefail

VERSION="${1:?ffmpeg version, e.g. 7.1.1}"
OUT="$(realpath -m "${2:?output directory}")"
WORK="${WORK:-$PWD/build-pack-transcode}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$WORK" "$OUT"
cd "$WORK"

TARBALL="ffmpeg-${VERSION}.tar.xz"
if [ ! -f "$TARBALL" ]; then
    curl -fsSLO "https://ffmpeg.org/releases/${TARBALL}"
    curl -fsSLO "https://ffmpeg.org/releases/${TARBALL}.asc" || true
fi
rm -rf "ffmpeg-${VERSION}"
tar xf "$TARBALL"
cd "ffmpeg-${VERSION}"

# x265.pc lists "-lgcc_s" in Libs.private. That name always resolves to the
# *dynamic* MinGW runtime and overrides -static-libgcc, so the shipped
# ffmpeg.exe would need libgcc_s_seh-1.dll on the user's machine. Point the
# link at the static libgcc instead, via a patched copy of the .pc file.
mkdir -p "$WORK/pc-override"
if [ -f /mingw64/lib/pkgconfig/x265.pc ]; then
    sed -e 's/-lgcc_s -lgcc -lgcc_s -lgcc/-lgcc -lgcc/' \
        /mingw64/lib/pkgconfig/x265.pc > "$WORK/pc-override/x265.pc"
    export PKG_CONFIG_PATH="$WORK/pc-override:${PKG_CONFIG_PATH:-}"
fi

# Scaling / cropping / pixel formats for resize, plus the composition filters
# needed to lay a transparent PNG onto a white background when the target format
# (JPEG, BMP) has no alpha channel: split + overlay, with color as the backdrop.
# palettegen/paletteuse give PNG a lossy mode: quantising to a palette is what
# pngquant and the online compressors do, and it is the only way a PNG gets
# dramatically smaller without changing its format.
FILTERS=scale,crop,pad,setsar,format,null,anull,transpose,hflip,vflip,select,fps,thumbnail,aresample,aformat,volume,atempo,trim,atrim,split,overlay,color,palettegen,paletteuse

# Encoders: the two GPL video encoders the author asked for, plus the LGPL/BSD
# ones for images and audio. AV1 uses libaom (BSD); SVT-AV1's API moved ahead
# of FFmpeg 7.1 and no longer compiles against it. Names are FFmpeg's internal
# ones (underscores: libaom_av1, libvpx_vp9).
ENCODERS=libx264,libx265,libaom_av1,libvpx_vp8,libvpx_vp9,libwebp,libwebp_anim,mpeg4,mjpeg,png,gif,tiff,bmp,rawvideo,wrapped_avframe,aac,libmp3lame,libopus,libvorbis,flac,ac3,pcm_s16le,pcm_f32le
MUXERS=mp4,mov,matroska,webm,ogg,mp3,wav,flac,ipod,adts,ac3,apng,gif,webp,image2,image2pipe,rawvideo,null,pcm_s16le,pcm_f32le

./configure \
    --prefix="$WORK/prefix" \
    --target-os=mingw32 --arch=x86_64 \
    --pkg-config-flags=--static \
    --extra-ldflags="-static -static-libgcc" \
    --enable-static --disable-shared \
    --disable-autodetect --enable-w32threads --enable-zlib --enable-libdav1d \
    --disable-everything \
    --disable-network --disable-devices --disable-hwaccels --disable-doc --disable-debug \
    --disable-ffplay --enable-ffmpeg --enable-ffprobe \
    --enable-gpl \
    --enable-libx264 --enable-libx265 --enable-libaom --enable-libvpx \
    --enable-libmp3lame --enable-libopus --enable-libvorbis --enable-libwebp \
    --enable-protocol=file,pipe \
    --enable-demuxers --enable-parsers --enable-decoders --enable-bsfs \
    --enable-encoder="$ENCODERS" \
    --enable-muxer="$MUXERS" \
    --enable-filter="$FILTERS" \
    --enable-swscale --enable-swresample \
    --extra-version=luling-transcode

# Hard guarantees, checked on the generated configuration. GPL is expected
# here (libx264/libx265); nonfree must never be on.
grep -q '^#define CONFIG_GPL 1' config.h
grep -q '^#define CONFIG_NONFREE 0' config.h
grep -q '^#define CONFIG_NETWORK 0' config.h
make -j"$JOBS"
make install
cp "$WORK/prefix/bin/ffmpeg.exe" "$WORK/prefix/bin/ffprobe.exe" "$OUT/"
strip "$OUT/ffmpeg.exe" "$OUT/ffprobe.exe"

# Licence material that has to travel with the binaries. x264 and x265 are GPL,
# so the source offer covers them as well.
cp COPYING.GPLv2 "$OUT/LICENSE-FFmpeg-GPL-2.0.txt"
cp COPYING.LGPLv2.1 "$OUT/LICENSE-FFmpeg-LGPL-2.1.txt"
X264_LICENSE="$(find /mingw64/share -ipath '*x264*' -iname 'COPYING*' | head -n1 || true)"
X265_LICENSE="$(find /mingw64/share -ipath '*x265*' -iname 'COPYING*' | head -n1 || true)"
[ -n "$X264_LICENSE" ] && cp "$X264_LICENSE" "$OUT/LICENSE-x264.txt"
[ -n "$X265_LICENSE" ] && cp "$X265_LICENSE" "$OUT/LICENSE-x265.txt"
cat > "$OUT/SOURCE.txt" <<EOF
Luling transcode pack

FFmpeg ${VERSION}, built from the unmodified release tarball:
  https://ffmpeg.org/releases/${TARBALL}

This build is licensed under the GNU General Public License version 2 or later
because it links the GPL components libx264 (H.264) and libx265 (H.265).
Corresponding source: the FFmpeg release tarball above plus
  libx264  https://code.videolan.org/videolan/x264
  libx265  https://bitbucket.org/multicoreware/x265_git
Luling runs this program as a separate process and does not link it.

Build script and configuration:
  packs/transcode/build.sh in the Luling repository

Licence texts ship next to the binaries (LICENSE-FFmpeg-GPL-2.0.txt,
LICENSE-FFmpeg-LGPL-2.1.txt, LICENSE-x264.txt, LICENSE-x265.txt).
EOF
"$OUT/ffmpeg.exe" -hide_banner -buildconf > "$OUT/BUILDCONF.txt" 2>&1

# The encoders the conversion features rely on must actually be present.
# Names here are ffmpeg's display names (hyphens), not the underscore names
# configure uses.
"$OUT/ffmpeg.exe" -hide_banner -encoders > "$WORK/pack-encoders.txt"
for encoder in libx264 libx265 libaom-av1 libvpx-vp9 libwebp mjpeg png aac libmp3lame libopus flac; do
    grep -Eq "[[:space:]]${encoder}[[:space:]]" "$WORK/pack-encoders.txt"
done
"$OUT/ffmpeg.exe" -hide_banner -muxers > "$WORK/pack-muxers.txt"
for muxer in mp4 matroska webm mp3 flac image2 webp; do
    grep -Eq "[[:space:]]${muxer}[[:space:]]" "$WORK/pack-muxers.txt"
done

# The filters the conversion paths use, including the ones that lay a
# transparent PNG onto a white background.
"$OUT/ffmpeg.exe" -hide_banner -filters > "$WORK/pack-filters.txt"
for filter in scale pad crop split overlay color palettegen paletteuse; do
    grep -Eq "[[:space:]]${filter}[[:space:]]" "$WORK/pack-filters.txt"
done

# The pack must run on a machine with no MinGW installed: no libgcc_s, no
# libstdc++, no libwinpthread. Checked here as well as in CI, because the
# local build is the one that ships.
for exe in ffmpeg.exe ffprobe.exe; do
    if objdump -p "$OUT/$exe" | grep -qiE 'DLL Name: (libgcc|libstdc|libwinpthread)'; then
        echo "$exe depends on a MinGW runtime DLL" >&2
        exit 1
    fi
done

echo "built: $(ls -la "$OUT")"
