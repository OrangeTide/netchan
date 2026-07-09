#!/bin/sh
# build.sh : build the vendored WebRTC stack and the nc_rtc gateway.
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#
# WebRTC data channels need DTLS (mbedtls), SCTP (usrsctp) and the SRTP key
# negotiation (libsrtp). Those are real build systems, not single files, so we
# build them with their own CMake into a local prefix (build/dist), then
# compile libpeer and the gateway against it with cc. This is the one part of
# the netchan-v2 demo that is not plain modular-make; it is native-only and
# opt-in. Requires: cmake, a C compiler, pthreads.

set -e
cd "$(dirname "$0")"

V="$PWD/vendor"
B="$PWD/build"
D="$B/dist"
J="$(nproc 2>/dev/null || echo 2)"
mkdir -p "$B"

build_dep() {
    name="$1"; src="$2"; shift 2
    if [ ! -f "$D/lib/lib$name.a" ] && [ ! -f "$D/lib/lib${name}2.a" ]; then
        echo ">> building $name"
        cmake -S "$src" -B "$B/$name" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DCMAKE_INSTALL_PREFIX="$D" "$@" >/dev/null
        cmake --build "$B/$name" -j"$J" >/dev/null
        cmake --install "$B/$name" >/dev/null
    fi
}

build_dep mbedtls "$V/mbedtls" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
build_dep usrsctp "$V/usrsctp" -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF
build_dep srtp    "$V/libsrtp" -DTEST_APPS=OFF -DBUILD_SHARED_LIBS=OFF

# libpeer (our patched copy) into a static lib
echo ">> building libpeer"
PEER_INC="-I$V/libpeer/include -I$V/libpeer/src -I$D/include -I$D/include/usrsctp"
mkdir -p "$B/libpeer"
for c in "$V"/libpeer/src/*.c; do
    o="$B/libpeer/$(basename "${c%.c}").o"
    cc -O2 -fPIC $PEER_INC -c "$c" -o "$o"
done
ar rcs "$B/libpeer.a" "$B"/libpeer/*.o

DEPS="$B/libpeer.a $D/lib/libusrsctp.a $D/lib/libsrtp2.a \
      $D/lib/libmbedtls.a $D/lib/libmbedx509.a $D/lib/libmbedcrypto.a"
LINK="-Wl,--start-group $DEPS -Wl,--end-group -lpthread -lm -ldl"
INC="-I$V/libpeer/include"

echo ">> building rtc_gateway, rtc_probe, udp_echo"
cc -O2 -Wall -D_GNU_SOURCE $INC rtc_gateway.c $LINK -o "$B/rtc_gateway"
cc -O2 -Wall -D_GNU_SOURCE $INC rtc_probe.c   $LINK -o "$B/rtc_probe"
cc -O2 -Wall udp_echo.c -o "$B/udp_echo"

echo "built: $B/rtc_gateway  $B/rtc_probe  $B/udp_echo"
