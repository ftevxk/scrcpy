#!/bin/bash
set -ex

case "$1" in
    32)
        WINXX=win32
        ;;
    64)
        WINXX=win64
        ;;
    *)
        echo "ERROR: $0 must be called with one argument: 32 or 64" >&2
        exit 1
        ;;
esac

cd "$(dirname ${BASH_SOURCE[0]})"
. build_common
cd .. # root project dir

WINXX_BUILD_DIR="$WORK_DIR/build-$WINXX"

app/deps/adb_windows.sh
# Use static linking for SDL3/dav1d/ffmpeg/libusb so that scrcpyw.exe has no
# external DLL dependencies. This is required for the single-file EXE
# distribution: the only binaries the user needs are scrcpy.exe/scrcpyw.exe.
app/deps/sdl.sh $WINXX cross static
app/deps/dav1d.sh $WINXX cross static
app/deps/ffmpeg.sh $WINXX cross static
app/deps/libusb.sh $WINXX cross static

DEPS_INSTALL_DIR="$PWD/app/deps/work/install/$WINXX-cross-static"
ADB_INSTALL_DIR="$PWD/app/deps/work/install/adb-windows"

# Copy adb binaries and scrcpy-server into app/data/ so the resource
# compiler (windres) can embed them into scrcpyw.exe via scrcpy-windows.rc.
# These files are referenced as "data/adb.exe" etc. relative to app/.
mkdir -p app/data
cp "$ADB_INSTALL_DIR"/adb.exe app/data/
cp "$ADB_INSTALL_DIR"/AdbWinApi.dll app/data/
cp "$ADB_INSTALL_DIR"/AdbWinUsbApi.dll app/data/

# scrcpy-server jar is built in a separate CI job (build-scrcpy-server).
# The caller must provide its path via $SCRCPY_SERVER_PATH, or it must be
# available at the conventional location.
if [[ -n "$SCRCPY_SERVER_PATH" && -f "$SCRCPY_SERVER_PATH" ]]; then
    cp "$SCRCPY_SERVER_PATH" app/data/scrcpy-server
elif [[ -f "$WORK_DIR/build-server/server/scrcpy-server" ]]; then
    cp "$WORK_DIR/build-server/server/scrcpy-server" app/data/scrcpy-server
else
    echo "ERROR: scrcpy-server not found. Set SCRCPY_SERVER_PATH or place it" \
         "at \$WORK_DIR/build-server/server/scrcpy-server" >&2
    exit 1
fi

# Never fall back to system libs
unset PKG_CONFIG_PATH
export PKG_CONFIG_LIBDIR="$DEPS_INSTALL_DIR/lib/pkgconfig"

rm -rf "$WINXX_BUILD_DIR"
meson setup "$WINXX_BUILD_DIR" \
    -Dc_args="-I$DEPS_INSTALL_DIR/include" \
    -Dc_link_args="-L$DEPS_INSTALL_DIR/lib" \
    --cross-file=cross_$WINXX.txt \
    --buildtype=release \
    --strip \
    -Db_lto=true \
    -Dcompile_server=false \
    -Dportable=true \
    -Dstatic=true
ninja -C "$WINXX_BUILD_DIR"

# Group intermediate outputs into a 'dist' directory.
# scrcpyw.exe is self-contained (adb + server embedded as resources).
# scrcpy.exe (console) still needs adb.exe + DLLs alongside it.
mkdir -p "$WINXX_BUILD_DIR/dist"
cp "$WINXX_BUILD_DIR"/app/scrcpy.exe "$WINXX_BUILD_DIR/dist/"
cp "$WINXX_BUILD_DIR"/app/scrcpyw.exe "$WINXX_BUILD_DIR/dist/"
cp "$ADB_INSTALL_DIR"/adb.exe "$WINXX_BUILD_DIR/dist/"
cp "$ADB_INSTALL_DIR"/AdbWinApi.dll "$WINXX_BUILD_DIR/dist/"
cp "$ADB_INSTALL_DIR"/AdbWinUsbApi.dll "$WINXX_BUILD_DIR/dist/"
cp app/data/scrcpy.png "$WINXX_BUILD_DIR/dist/"
cp app/data/disconnected.png "$WINXX_BUILD_DIR/dist/"

# Clean up the embedded binaries from app/data/ so they don't pollute the
# source tree or get committed accidentally.
rm -f app/data/adb.exe app/data/AdbWinApi.dll app/data/AdbWinUsbApi.dll app/data/scrcpy-server
