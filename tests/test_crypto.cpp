// test_crypto.cpp — the platform crypto backends against published vectors.
//
// There are three backends: CNG on Windows, CommonCrypto on macOS, OpenSSL
// elsewhere. Each is chosen so the plugin never makes a church install a
// library, and each is a place where a plausible-looking mistake produces
// plausible-looking output.
//
// That matters more here than the small amount of code suggests. Everything
// the signer produces is derived from these two functions, and a wrong digest
// does not fail loudly — it yields a well-formed request that the store rejects
// with SignatureDoesNotMatch, which reads exactly like a mistyped secret key.
// An operator would sooner re-enter their credentials a dozen times than
// suspect the hash. So each backend is checked against values published by
// somebody else rather than against itself.
//
// SHA-256 vectors: FIPS 180-2 / NIST examples.
// HMAC-SHA256 vectors: RFC 4231.
#include "../src/core/crypto.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::string hex(const std::vector<uint8_t>& v) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s.push_back(hx[b >> 4]); s.push_back(hx[b & 0xF]); }
    return s;
}

static std::vector<uint8_t> rep(uint8_t byte, size_t n) {
    return std::vector<uint8_t>(n, byte);
}

static void check_sha(const std::string& in, const char* expect, const char* what) {
    const std::string got =
        crypto::sha256_hex(reinterpret_cast<const uint8_t*>(in.data()), in.size());
    const bool ok = (got == expect);
    if (!ok) {
        std::printf("  [FAIL] %s\n         got      %s\n         expected %s\n",
                    what, got.c_str(), expect);
        ++g_fail;
    } else {
        std::printf("  [ok]   %s\n", what);
    }
}

static void check_hmac(const std::vector<uint8_t>& key, const std::string& msg,
                       const char* expect, const char* what) {
    const std::string got = hex(crypto::hmac_sha256(key, msg));
    const bool ok = (got == expect);
    if (!ok) {
        std::printf("  [FAIL] %s\n         got      %s\n         expected %s\n",
                    what, got.c_str(), expect);
        ++g_fail;
    } else {
        std::printf("  [ok]   %s\n", what);
    }
}

int main() {
#if defined(_WIN32)
    std::printf("backend: CNG (bcrypt)\n");
#elif defined(__APPLE__)
    std::printf("backend: CommonCrypto\n");
#else
    std::printf("backend: OpenSSL\n");
#endif

    std::printf("SHA-256 (NIST vectors)\n");
    check_sha("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "the empty string");
    check_sha("abc",
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "\"abc\"");
    check_sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "the 448-bit multi-block message");

    // A megabyte, to exercise anything that chunks internally rather than
    // hashing in one call.
    {
        std::string big(1024 * 1024, 'a');
        auto a = crypto::sha256(reinterpret_cast<const uint8_t*>(big.data()), big.size());
        auto b = crypto::sha256(reinterpret_cast<const uint8_t*>(big.data()), big.size());
        CHECK(a.size() == 32 && a == b, "a megabyte hashes to 32 stable bytes");
    }

    std::printf("HMAC-SHA256 (RFC 4231)\n");
    check_hmac(rep(0x0b, 20), "Hi There",
               "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
               "case 1: 20-byte key");

    {
        const std::string k = "Jefe";
        check_hmac(std::vector<uint8_t>(k.begin(), k.end()),
                   "what do ya want for nothing?",
                   "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                   "case 2: short ASCII key");
    }

    check_hmac(rep(0xaa, 20), std::string(50, (char)0xdd),
               "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
               "case 3: binary data");

    // Keys longer than the 64-byte block are hashed first. Getting this wrong
    // is the classic HMAC implementation bug, and AWS signing keys are exactly
    // 32 bytes so it would never show up in normal use.
    check_hmac(rep(0xaa, 131),
               "Test Using Larger Than Block-Size Key - Hash Key First",
               "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
               "case 6: key longer than the block size");

    check_hmac(rep(0xaa, 131),
               "This is a test using a larger than block-size key and a larger "
               "than block-size data. The key needs to be hashed before being "
               "used by the HMAC algorithm.",
               "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
               "case 7: long key and long data");

    std::printf("Consistency\n");
    {
        const std::string s = "obs-multisite";
        auto raw = crypto::sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        CHECK(crypto::sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size())
                  == hex(raw),
              "sha256_hex agrees with sha256, lowercase");
        CHECK(crypto::hmac_sha256({}, "").size() == 32,
              "an empty key and message still yield 32 bytes");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL CRYPTO TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
