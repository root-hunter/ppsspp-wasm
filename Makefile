.PHONY: wasm-dev-config wasm-dev-build wasm-dev wasm-release-config wasm-release-build wasm-release wasm-config wasm-build config-wasm build-wasm ffmpeg-wasm ffmpeg-wasm-clean

WASM_DEV_BUILD_DIR ?= build-wasm
WASM_RELEASE_BUILD_DIR ?= build-wasm-release
WASM_JOBS ?= -j
WASM_MAIN_LOOP_FPS ?= 60
WASM_INITIAL_MEMORY ?= 536870912
WASM_MAXIMUM_MEMORY ?= 4294967296
CMAKE ?= $(or $(wildcard /usr/bin/cmake),cmake)

# FFmpeg WASM output directory (built from the ffmpeg/ submodule)
FFMPEG_WASM_PREFIX := $(abspath ffmpeg/wasm)
FFMPEG_WASM_SENTINEL := $(FFMPEG_WASM_PREFIX)/lib/libavcodec.a

WASM_COMMON_CMAKE_ARGS := \
	-G Ninja \
	-DUSING_GLES2=ON \
	-DUSING_EGL=OFF \
	-DVULKAN=OFF \
	-DUSE_NO_MMAP=ON \
	-DUSE_FFMPEG=ON \
	-DFFMPEG_DIR=$(FFMPEG_WASM_PREFIX) \
	-DUSE_DISCORD=OFF \
	-DUSE_MINIUPNPC=OFF \
	-DUSE_SYSTEM_LIBSDL2=OFF \
	-DUSE_SYSTEM_LIBPNG=OFF \
	-DUSE_SYSTEM_FREETYPE=OFF \
	-DUSE_SYSTEM_LIBZIP=OFF \
	-DWASM_MAIN_LOOP_FPS=$(WASM_MAIN_LOOP_FPS) \
	-DWASM_INITIAL_MEMORY=$(WASM_INITIAL_MEMORY) \
	-DWASM_MAXIMUM_MEMORY=$(WASM_MAXIMUM_MEMORY)

# ── FFmpeg cross-compile for WASM ─────────────────────────────────────────────
# Builds the ppsspp-ffmpeg submodule into static .a files using Emscripten.
# Only runs when the output sentinel file is missing (i.e. first build or after
# ffmpeg-wasm-clean).  The codec/demuxer set mirrors the existing platform
# build scripts (linux_x86-64.sh etc.) with the extra Emscripten threading
# flags required to link against PPSSPP's -pthread build.
$(FFMPEG_WASM_SENTINEL):
	@echo "==> Building FFmpeg static libs for WebAssembly..."
	@test -f ffmpeg/configure || (echo "ERROR: ffmpeg submodule not initialised. Run: git submodule update --init --recursive ffmpeg" >&2; exit 1)
	@which emcc >/dev/null 2>&1 || (echo "ERROR: emcc not found. Run: source ~/emsdk/emsdk_env.sh" >&2; exit 1)
	cd ffmpeg && \
	make distclean 2>/dev/null || true && \
	rm -f config.h && \
	emconfigure ./configure \
		--prefix="$(FFMPEG_WASM_PREFIX)" \
		--cc=emcc \
		--cxx=em++ \
		--ar=emar \
		--ranlib=emranlib \
		--nm=llvm-nm \
		--target-os=none \
		--arch=x86_32 \
		--cpu=generic \
		--disable-shared \
		--enable-static \
		--disable-runtime-cpudetect \
		--disable-asm \
		--disable-inline-asm \
		--disable-pthreads \
		--disable-w32threads \
		--disable-os2threads \
		--disable-debug \
		--disable-stripping \
		--disable-avdevice \
		--disable-avfilter \
		--disable-postproc \
		--disable-filters \
		--disable-programs \
		--disable-network \
		--disable-encoders \
		--disable-doc \
		--disable-ffplay \
		--disable-ffprobe \
		--disable-ffserver \
		--disable-ffmpeg \
		--disable-iconv \
		--disable-bzlib \
		--disable-lzma \
		--disable-everything \
		--enable-decoder=h264 \
		--enable-decoder=mpeg4 \
		--enable-decoder=h263 \
		--enable-decoder=h263p \
		--enable-decoder=mpeg2video \
		--enable-decoder=mjpeg \
		--enable-decoder=mjpegb \
		--enable-decoder=aac \
		--enable-decoder=aac_latm \
		--enable-decoder=atrac3 \
		--enable-decoder=atrac3p \
		--enable-decoder=mp3 \
		--enable-decoder=pcm_s16le \
		--enable-decoder=pcm_s8 \
		--enable-encoder=ffv1 \
		--enable-encoder=huffyuv \
		--enable-encoder=mpeg4 \
		--enable-encoder=pcm_s16le \
		--enable-demuxer=h264 \
		--enable-demuxer=h263 \
		--enable-demuxer=m4v \
		--enable-demuxer=mpegps \
		--enable-demuxer=mpegvideo \
		--enable-demuxer=avi \
		--enable-demuxer=mp3 \
		--enable-demuxer=aac \
		--enable-demuxer=pmp \
		--enable-demuxer=oma \
		--enable-demuxer=pcm_s16le \
		--enable-demuxer=pcm_s8 \
		--enable-demuxer=wav \
		--enable-muxer=avi \
		--enable-parser=h264 \
		--enable-parser=mpeg4video \
		--enable-parser=mpegvideo \
		--enable-parser=aac \
		--enable-parser=aac_latm \
		--enable-parser=mpegaudio \
		--enable-protocol=file \
		--enable-zlib \
		--disable-yasm \
		'--extra-cflags=-D__STDC_CONSTANT_MACROS -O2 -msimd128 -pthread -mbulk-memory -matomics' && \
	emmake make $(WASM_JOBS) install
	@echo "==> FFmpeg WASM libs ready at $(FFMPEG_WASM_PREFIX)/lib/"

ffmpeg-wasm: $(FFMPEG_WASM_SENTINEL)

ffmpeg-wasm-clean:
	rm -rf $(FFMPEG_WASM_PREFIX)
	cd ffmpeg && make distclean 2>/dev/null || true

# ── PPSSPP WASM build targets ─────────────────────────────────────────────────
wasm-dev-config: $(FFMPEG_WASM_SENTINEL)
	emcmake $(CMAKE) -S . -B $(WASM_DEV_BUILD_DIR) $(WASM_COMMON_CMAKE_ARGS) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWASM_MAX_PERF=OFF -DWASM_ENABLE_LTO=OFF -DWASM_MALLOC=emmalloc

wasm-dev-build:
	$(CMAKE) --build $(WASM_DEV_BUILD_DIR) $(WASM_JOBS)

wasm-dev: wasm-dev-config wasm-dev-build

wasm-release-config: $(FFMPEG_WASM_SENTINEL)
	emcmake $(CMAKE) -S . -B $(WASM_RELEASE_BUILD_DIR) $(WASM_COMMON_CMAKE_ARGS) -DCMAKE_BUILD_TYPE=Release -DWASM_MAX_PERF=ON -DWASM_ENABLE_LTO=OFF -DWASM_MALLOC=mimalloc

wasm-release-build:
	$(CMAKE) --build $(WASM_RELEASE_BUILD_DIR) $(WASM_JOBS)

wasm-release: wasm-release-config wasm-release-build

wasm-config: wasm-dev-config
wasm-build: wasm-dev-build
config-wasm: wasm-dev-config
build-wasm: wasm-dev-build

docker:
	docker run --rm -it \
	-v $(CURDIR):/src \
	-w /src \
	emscripten/emsdk:latest \
	bash
