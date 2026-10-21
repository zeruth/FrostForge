#include <cstdint>
#include <cstring>

// The Ed25519ctx signature check the world connection's ENTER_ENCRYPTED_MODE goes through
// (FUN_141561000 -> FUN_1420bf840 -> here). Ported by hand because the decompilation lost one
// statement: after FUN_1420bee80 writes the result point's x to a 32-byte block, the reference
// copies the block (two movups) to the buffer it then tests and negates (rbp - 0x40); Ghidra typed
// the source as a 4-byte local and dropped the copy as dead, so the translated check tested a
// buffer nothing wrote and refused every server signature (disconnect reason 24, 2026-10-20).
// Everything it calls stays translated; the curve operations behind `curve` run as the
// reference's own code (tools/fidelity_exceptions.json "native").

extern "C" {
unsigned long long FUN_1420c8930(...);   // SHA-512 update (ctx, data, len)
unsigned long long FUN_1420c8840(...);   // SHA-512 final (ctx, out64)
unsigned char FUN_1420bf0c0(...);        // decode R (x out, y out, encoded, curve)
unsigned char FUN_1420c8350(...);        // bytes to a little-endian bignum (out, words, in, len, scratch)
int FUN_1420c80a0(...);                  // bignum compare (a, words, b, words)
unsigned int FUN_1420beaa0(...);         // reduce a 512-bit hash mod L (out, in)
unsigned char FUN_1420c81f0(...);        // bignum to bytes (out, len, in, words, scratch)
unsigned long long FUN_1420bf000(...);   // the base point (out, x, y, curve)
unsigned int FUN_1420bee80(...);         // a point to affine x and y (x out, y out, point, curve)
int FUN_1420c8400(...);                  // bignum is zero (a, words)
unsigned long long FUN_1420c86b0(...);   // bignum subtract (a -= b: a, words, b, words, borrow)
}

using CurveOp = unsigned long long (*)(...);

// (kept by the linker until the translator calls it: unreferenced, /OPT:REF dropped it from the
// PDB, tools/fidelity.py found no function under the tag, and the callers stayed on the
// translation)
#pragma comment(linker, "/include:Ed25519VerifyCore")

// ref: FUN_1420bf4f0
extern "C" unsigned char Ed25519VerifyCore(const uint8_t* sig, void* sha512, const uint8_t* publicKey,
    const uint64_t* keyX, const uint64_t* keyY, unsigned char (*hashMessage)(void*, void*), void* message,
    const uint8_t* curve) {
    alignas(16) uint8_t hash[64] = {};          // local_218: SHA-512(dom2 || R || A || M)
    alignas(16) uint8_t rx[32] = {}, ry[32] = {};   // local_3f8, local_3d8: R decoded
    alignas(16) uint64_t s[4] = {};             // local_458: S
    alignas(16) uint64_t h[8] = {};             // local_1d8: the hash as a 512-bit number
    alignas(16) uint64_t k[4] = {};             // local_438: the hash reduced mod L
    alignas(16) uint8_t kBytes[32] = {};        // local_418
    alignas(16) uint8_t base[192] = {};         // local_198: the base point
    alignas(16) uint64_t ax[4] = {}, ay[4] = {}, az[4] = {};   // blk_398, blk_368, blk_338
    // local_2d8 .. local_248: A as one extended point (X, Y, Z, T, 0x30 each, in that order), which
    // the curve's double scalar multiply (+0x60) takes whole through its first coordinate
    alignas(16) uint8_t pointA[4 * 48] = {};
    uint8_t* fx = pointA;
    uint8_t* fy = pointA + 48;
    uint8_t* fz = pointA + 96;
    uint8_t* ft = pointA + 144;
    alignas(16) uint8_t point[192] = {};        // local_d8: [S]B - [k]A
    alignas(16) uint64_t outX[4] = {};          // the result point's x as FUN_1420bee80 leaves it
    alignas(16) uint8_t outY[32] = {};          // local_3b8
    alignas(16) uint64_t xCopy[4] = {};         // auStack_478 (rbp - 0x40)
    alignas(16) uint64_t negated[4] = {};       // uStack_4c8
    uint64_t scratch = 0;                       // the home slot the reference lends its callees

    FUN_1420c8930(sha512, sig, 0x20);
    FUN_1420c8930(sha512, publicKey, 0x20);
    if (!hashMessage(sha512, message)) {
        return 0;
    }
    FUN_1420c8840(sha512, hash);
    if (!FUN_1420bf0c0(rx, ry, sig, curve)
        || !FUN_1420c8350(s, 4, sig + 0x20, 0x20, &scratch)
        || !(FUN_1420c80a0(s, 4, reinterpret_cast<void*>(0x1428e1dd8ull), 4) < 0)
        || !FUN_1420c8350(h, 8, hash, 0x40, &scratch)) {
        return 0;
    }
    FUN_1420beaa0(k, h);
    if (!FUN_1420c81f0(kBytes, 0x20, k, 4, &scratch)) {
        return 0;
    }
    FUN_1420bf000(base, reinterpret_cast<void*>(0x1428e1d50ull), reinterpret_cast<void*>(0x1428e1d30ull), curve);
    std::memcpy(ax, keyX, 32);
    std::memcpy(ay, keyY, 32);
    az[0] = 1;
    auto op = [curve](size_t slot) { return *reinterpret_cast<const CurveOp*>(curve + slot); };
    op(0x10)(fx, ax);
    op(0x10)(fy, ay);
    op(0x10)(fz, az);
    op(0x18)(ft, fx, fy);
    op(0x60)(point, base, sig + 0x20, fx, kBytes, 0xfdull);
    FUN_1420bee80(outX, outY, point, curve);
    std::memcpy(xCopy, outX, 32);     // the copy the decompilation dropped
    uint64_t x[4] = {};
    if (FUN_1420c8400(xCopy, 4) == 0) {
        std::memcpy(negated, reinterpret_cast<const void*>(0x1428e1db8ull), 32);
        FUN_1420c86b0(negated, 4, xCopy, 4, 0);
        std::memcpy(x, negated, 32);
    }
    return std::memcmp(x, rx, 0x20) == 0 && std::memcmp(outY, ry, 0x20) == 0 ? 1 : 0;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420beaa0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bee80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bf000=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420bf0c0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c80a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c81f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c8350=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c8400=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c86b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c8840=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420c8930=GenForwardMissing")
// ---- end of link fallbacks ----
