#!/usr/bin/env bash
# Native transport test suite - builds the C++ test CLIs and runs every check
# with no human input. Byte-compatibility with the browser transport is proven
# against nostr-tools + WebCrypto over the local NIP-01 relay.
#
#   NN_PREFIX  local dev prefix for deps not installed system-wide
#              (libdatachannel/secp256k1 headers+libs). Default /tmp/localdev.
#              On a box with the -devel packages installed, run: NN_PREFIX=/usr
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PREFIX="${NN_PREFIX:-/tmp/localdev}"
OUT=/tmp/nn
mkdir -p "$OUT"

INC="-I $ROOT/platform/native -I $ROOT/source/build/include -I $PREFIX/usr/include -I $PREFIX/include"
LIBDIRS="-L $PREFIX/lib -L $PREFIX/usr/lib64 -L $PREFIX/lib64 -Wl,-rpath,$PREFIX/usr/lib64 -Wl,-rpath,$PREFIX/lib64"
CXX="${CXX:-g++} -std=c++17 -O2 -Wall"

echo "== build native transport test CLIs (prefix=$PREFIX) =="
$CXX $INC "$ROOT/platform/native/test/nn_crypto_test.cpp" -o "$OUT/crypto_test" $LIBDIRS -lsecp256k1 -lcrypto
$CXX $INC "$ROOT/platform/native/test/nn_nostr_test.cpp"  -o "$OUT/nostr_test"  $LIBDIRS -lsecp256k1 -lcrypto
# relay transport now runs over libcurl (OpenSSL WebSocket), not libdatachannel.
$CXX $INC "$ROOT/platform/native/test/nn_relay_test.cpp"  -o "$OUT/relay_test"  $LIBDIRS -lcurl -lsecp256k1 -lcrypto -lpthread
$CXX $INC "$ROOT/platform/native/test/nn_peer_test.cpp"   -o "$OUT/peer_test"   $LIBDIRS -ldatachannel -lsecp256k1 -lcrypto -lpthread
# seam test links the REAL net_transport_native.cpp (needs -DNETNATIVE + the engine include dir)
$CXX -DNETNATIVE $INC -I "$ROOT/source/duke3d/src" \
     "$ROOT/platform/native/test/nn_seam_test.cpp" "$ROOT/source/duke3d/src/net_transport_native.cpp" \
     -o "$OUT/seam_test" $LIBDIRS -ldatachannel -lsecp256k1 -lcrypto -lpthread

export LD_LIBRARY_PATH="$PREFIX/usr/lib64:$PREFIX/lib64:${LD_LIBRARY_PATH:-}"

echo "== crypto selftest =="
"$OUT/crypto_test" selftest
echo "== crypto interop vs nostr-tools/WebCrypto =="
node "$ROOT/platform/native/test/crypto_check.mjs" "$OUT/crypto_test"
echo "== nostr event interop vs nostr-tools verifyEvent =="
node "$ROOT/platform/native/test/nostr_check.mjs" "$OUT/nostr_test"
echo "== relay end-to-end interop (native <-> browser stack) =="
node "$ROOT/platform/native/test/relay_e2e.mjs" "$OUT/relay_test"
echo "== WebRTC two-peer connect (presence -> offer/answer -> 3 channels) =="
node "$ROOT/platform/native/test/peer_e2e.mjs" "$OUT/peer_test"
echo "== seam end-to-end: join handshake + frames through net_transport.h =="
node "$ROOT/platform/native/test/seam_e2e.mjs" "$OUT/seam_test"

echo
echo "ALL NATIVE TRANSPORT TESTS PASSED"
