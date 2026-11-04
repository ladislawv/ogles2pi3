#!/bin/bash
# Builds dEQP-GLES2 for Linux on the Raspberry Pi 3 (aarch64, Mesa's vc4): the
# hardware's own baseline (PI3_PLAN P0, §6.1). Run in WSL:
#   bash tests/pi3/deqp/build.sh
# The same VK-GL-CTS, zlib and libpng as ports/deqp (run ports/deqp/build.sh
# once: it clones ~/vk-gl-cts and downloads ~/deqp-deps), and the same
# sources as its build.sh lists, compiled by this script rather than the CTS's
# CMake (which wants SPIRV-Tools, glslang and Vulkan's layers present just to
# configure), with Ubuntu's aarch64-linux-gnu g++ 14. In place of the Amiga
# parts: delibs' own Unix layer and tcuPi3Platform.cpp (an EGL pbuffer on
# Mesa's surfaceless platform). The Pi's EGL and GLES headers and libraries
# are copied from the Pi ($PI, an ssh host) into $SYSROOT. The CTS's sources
# are read where they are (ports/deqp's patches are for the 68k and for
# big-endian hosts). The result is $OUT/run: deqp-gles2, its data and the
# must-pass list; tests/pi3/deqp/run.sh copies it to the Pi and runs it.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PI=${PI:-pi3}
CTS_SRC=${CTS_SRC:-$HOME/vk-gl-cts}
DEPS=${DEPS:-$HOME/deqp-deps}
SYSROOT=${SYSROOT:-$HOME/pi3-sysroot}
OUT=${OUT:-$HOME/ogles2-build/pi3/deqp}
J=${J:-16}
CXX=aarch64-linux-gnu-g++-14
CC=aarch64-linux-gnu-gcc-14
CTS_COMMIT=067e8832315e79817ede1c4863804e440f5d1c80

[ "$(git -C "$CTS_SRC" rev-parse HEAD)" = "$CTS_COMMIT" ] ||
    { echo "$CTS_SRC is not at opengl-es-cts-3.2.14.1 (run ports/deqp/build.sh once)"; exit 1; }
[ -f "$DEPS/zlib/zlib.h" ] && [ -f "$DEPS/libpng/png.h" ] && [ -f "$DEPS/libpng/pnglibconf.h" ] ||
    { echo "no zlib/libpng in $DEPS (run ports/deqp/build.sh once)"; exit 1; }
command -v "$CXX" >/dev/null || { echo "no $CXX"; exit 1; }

# The Pi's EGL and GLES: headers, and glvnd's libEGL and libGLESv2 to link
# against (they need libGLdispatch).
if [ ! -f "$SYSROOT/lib/libEGL.so" ]; then
    mkdir -p "$SYSROOT/include" "$SYSROOT/lib"
    ssh "$PI" 'cd /usr/include && tar -cf - EGL GLES2 GLES3 KHR' | tar -xf - -C "$SYSROOT/include"
    for l in libEGL.so.1 libGLESv2.so.2 libGLdispatch.so.0; do
        scp -q "$PI:/usr/lib/aarch64-linux-gnu/$l" "$SYSROOT/lib/$l.tmp"
        mv "$SYSROOT/lib/$l.tmp" "$SYSROOT/lib/$l"
    done
    ln -sf libGLESv2.so.2 "$SYSROOT/lib/libGLESv2.so"
    ln -sf libEGL.so.1 "$SYSROOT/lib/libEGL.so"
fi

ROOT=$CTS_SRC
OBJ=$OUT/obj
mkdir -p "$OBJ"
rm -f "$OBJ/FAILED"

F=$ROOT/framework
D=$F/delibs
INC="-I$D/debase -I$D/depool -I$D/dethread -I$D/destream -I$D/deutil -I$D/decpp -I$F/qphelper
-I$F/common -I$F/xexml -I$F/opengl -I$F/opengl/wrapper -I$F/opengl/simplereference
-I$F/referencerenderer -I$F/randomshaders -I$ROOT/modules/glshared -I$ROOT/modules/gles2
-I$ROOT/modules/gles2/accuracy -I$ROOT/modules/gles2/functional -I$ROOT/modules/gles2/performance
-I$ROOT/modules/gles2/stress -I$DEPS/libpng -I$DEPS/zlib -I$F/egl -I$F/egl/wrapper
-I$SYSROOT/include"
DEFS="-DDE_OS=DE_OS_UNIX -DDE_CPU=DE_CPU_ARM_64 -DDE_PTR_SIZE=8 -DDE_COMPILER=DE_COMPILER_GCC -DNDEBUG
-D_GNU_SOURCE -DDEQP_TARGET_NAME=\"Pi3Linux\" -DDEQP_SUPPORT_GLES2=1 -DQP_SUPPORT_PNG -DPNG_ARM_NEON_OPT=0"
FLAGS="-O2 -fno-strict-aliasing -pthread"
CXXFLAGS="-std=gnu++17 $FLAGS $DEFS $INC"
CFLAGS="-std=gnu99 $FLAGS $DEFS $INC -DZ_HAVE_UNISTD_H"

# Sources, relative to $ROOT: a directory, then files in it; ports/deqp's
# lists with the Amiga layer replaced by delibs' Unix one.
expand() {
    local d files f
    while read -r d files; do
        for f in $files; do echo "$d/$f"; done
    done
}
C_SRC=$(expand <<'EOF'
framework/delibs/debase deDefs.c deFloat16.c deInt32.c deMath.c deMemory.c deRandom.c deString.c
framework/delibs/debase deSha1.c
framework/delibs/depool deMemPool.c dePoolArray.c dePoolHeap.c dePoolHash.c dePoolHashArray.c
framework/delibs/depool dePoolHashSet.c dePoolMultiSet.c dePoolSet.c dePoolStringBuilder.c
framework/delibs/dethread deAtomic.c deSingleton.c
framework/delibs/dethread/unix deMutexUnix.c deSemaphoreUnix.c deThreadLocalUnix.c deThreadUnix.c
framework/delibs/deutil deClock.c deCommandLine.c deDynamicLibrary.c deFile.c deProcess.c deSocket.c deTimer.c
framework/qphelper qpCrashHandler.c qpDebugOut.c qpInfo.c qpTestLog.c qpWatchDog.c qpXmlWriter.c
EOF
)
DEP_SRC=$(expand <<'EOF'
zlib adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c
zlib inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
libpng png.c pngset.c pngget.c pngrutil.c pngtrans.c pngwutil.c pngread.c pngrio.c pngwio.c
libpng pngwrite.c pngrtran.c pngwtran.c pngmem.c pngerror.c pngpread.c
EOF
)
CXX_SRC="$(expand <<'EOF'
framework/delibs/decpp deAppendList.cpp deArrayBuffer.cpp deArrayUtil.cpp deBlockBuffer.cpp
framework/delibs/decpp deCommandLine.cpp deDefs.cpp deDirectoryIterator.cpp deDynamicLibrary.cpp
framework/delibs/decpp deFilePath.cpp deMemPool.cpp deMeta.cpp deMutex.cpp dePoolArray.cpp
framework/delibs/decpp dePoolString.cpp deRandom.cpp deRingBuffer.cpp deSemaphore.cpp deSTLUtil.cpp
framework/delibs/decpp deSharedPtr.cpp deStringUtil.cpp deThread.cpp deThreadLocal.cpp
framework/delibs/decpp deThreadSafeRingBuffer.cpp deUniquePtr.cpp deSpinBarrier.cpp deSha1.cpp
framework/xexml xeDefs.cpp xeXMLParser.cpp
framework/common tcuApp.cpp tcuArray.cpp tcuBilinearImageCompare.cpp tcuCommandLine.cpp
framework/common tcuCompressedTexture.cpp tcuDefs.cpp tcuFloat.cpp tcuFloatFormat.cpp
framework/common tcuFuzzyImageCompare.cpp tcuImageCompare.cpp tcuImageIO.cpp tcuInterval.cpp
framework/common tcuMatrix.cpp tcuPlatform.cpp tcuRGBA.cpp tcuRandomValueIterator.cpp
framework/common tcuRenderTarget.cpp tcuResource.cpp tcuResultCollector.cpp tcuSurface.cpp
framework/common tcuSurfaceAccess.cpp tcuTestCase.cpp tcuTestContext.cpp tcuTestSessionExecutor.cpp
framework/common tcuTestLog.cpp tcuTestPackage.cpp tcuTexture.cpp tcuTextureUtil.cpp
framework/common tcuFunctionLibrary.cpp tcuThreadUtil.cpp tcuStringTemplate.cpp tcuTexLookupVerifier.cpp
framework/common tcuTexCompareVerifier.cpp tcuTexVerifierUtil.cpp tcuCPUWarmup.cpp
framework/common tcuFactoryRegistry.cpp tcuSeedBuilder.cpp tcuMaybe.cpp tcuEither.cpp
framework/common tcuTestHierarchyIterator.cpp tcuTestHierarchyUtil.cpp tcuAstcUtil.cpp
framework/common tcuRasterizationVerifier.cpp tcuWaiverUtil.cpp
framework/opengl gluContextInfo.cpp gluDefs.cpp gluDrawUtil.cpp gluFboRenderContext.cpp
framework/opengl gluPixelTransfer.cpp gluProgramInterfaceQuery.cpp gluRenderConfig.cpp
framework/opengl gluRenderContext.cpp gluShaderProgram.cpp gluShaderUtil.cpp gluStateReset.cpp
framework/opengl gluTexture.cpp gluTextureUtil.cpp gluTextureTestUtil.cpp gluVarType.cpp
framework/opengl gluVarTypeUtil.cpp gluStrUtil.cpp gluCallLogWrapper.cpp gluObjectWrapper.cpp
framework/opengl gluContextFactory.cpp gluDummyRenderContext.cpp gluPlatform.cpp gluShaderLibrary.cpp
framework/opengl/wrapper glwDefs.cpp glwFunctions.cpp glwInitFunctions.cpp glwWrapper.cpp
framework/opengl/simplereference sglrContext.cpp sglrContextUtil.cpp sglrContextWrapper.cpp
framework/opengl/simplereference sglrReferenceContext.cpp sglrReferenceUtils.cpp sglrShaderProgram.cpp
framework/opengl/simplereference sglrGLContext.cpp
framework/referencerenderer rrDefs.cpp rrFragmentOperations.cpp rrMultisamplePixelBufferAccess.cpp
framework/referencerenderer rrPrimitivePacket.cpp rrRasterizer.cpp rrRenderer.cpp rrShaders.cpp
framework/referencerenderer rrShadingContext.cpp rrVertexAttrib.cpp rrVertexPacket.cpp
framework/platform tcuMain.cpp
EOF
)
$(cd "$ROOT" && ls framework/randomshaders/rsg*.cpp | grep -v rsgTest.cpp)
$(cd "$ROOT" && ls modules/glshared/*.cpp modules/gles2/*.cpp modules/gles2/accuracy/*.cpp \
    modules/gles2/functional/*.cpp modules/gles2/performance/*.cpp modules/gles2/stress/*.cpp)"

# Compiles what changed, in up to $J jobs; a failure leaves its log beside its
# object. A change to this script or the platform recompiles everything.
NEWEST=$(ls -t "$HERE/build.sh" "$HERE/tcuPi3Platform.cpp" | head -1)
cc_one() { # compiler flags source-root source
    local o=$OBJ/$(echo "$4" | tr / _).o
    o=${o%.*}.o
    [ "$o" -nt "$3/$4" ] && [ "$o" -nt "$NEWEST" ] && return 0
    $1 $2 -c "$3/$4" -o "$o" 2> "$o.log" || { echo "$4" >> "$OBJ/FAILED"; rm -f "$o"; }
}
run() {
    while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n || true; done
    cc_one "$@" &
}
for f in $CXX_SRC; do run "$CXX" "$CXXFLAGS" "$ROOT" "$f"; done
for f in $C_SRC; do run "$CC" "$CFLAGS" "$ROOT" "$f"; done
for f in $DEP_SRC; do run "$CC" "$CFLAGS" "$DEPS" "$f"; done
run "$CXX" "$CXXFLAGS" "$HERE" tcuPi3Platform.cpp
wait
if [ -f "$OBJ/FAILED" ]; then
    echo "FAILED ($(wc -l < "$OBJ/FAILED")):"
    cat "$OBJ/FAILED"
    exit 1
fi

mkdir -p "$OUT/run"
"$CXX" -pthread -o "$OUT/run/deqp-gles2" "$OBJ"/*.o -L"$SYSROOT/lib" -Wl,-rpath-link,"$SYSROOT/lib" \
    -lEGL -lGLESv2 -ldl -lm -static-libstdc++ -static-libgcc
aarch64-linux-gnu-strip "$OUT/run/deqp-gles2"
rm -rf "$OUT/run/gles2"
mkdir -p "$OUT/run/gles2"
cp -r "$ROOT/data/gles2/data" "$ROOT/data/gles2/shaders" "$OUT/run/gles2/"
cp "$ROOT/external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main/gles2-main.txt" "$OUT/run/"
ls -l "$OUT/run"
