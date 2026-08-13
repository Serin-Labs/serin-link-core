/* Host tests: the vendored Monocypher build that backs the ESPHome component's
 * sl2_crypto_t hooks, checked against the standards' own vectors.
 *
 * The dial signs and derives with libsodium; the controller now does it with
 * Monocypher. Nothing in the wire format changed, so the only thing that can
 * break interop is one side disagreeing with RFC 8032 / RFC 7748 -- which is
 * exactly what these vectors pin. The secret-key layout assertion matters just
 * as much: identity keys provisioned in NVS by the libsodium build are
 * seed||public_key, and a build that laid them out differently would silently
 * fail to re-derive an existing identity after an update. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

static void unhex(const char *hex, uint8_t *out, size_t out_len) {
    assert(strlen(hex) == out_len * 2);
    for (size_t i = 0; i < out_len; i++) {
        unsigned byte;
        assert(sscanf(hex + i * 2, "%2x", &byte) == 1);
        out[i] = (uint8_t) byte;
    }
}

/* RFC 8032 s7.1. seed/public/message/signature, exactly as the RFC prints
 * them; "SECRET KEY" there is the 32-byte seed, not our 64-byte private key. */
struct ed_vector {
    const char *name;
    const char *seed;
    const char *pub;
    const char *msg;                        /* hex, "" = empty message */
    const char *sig;
};

static const struct ed_vector ED_VECTORS[] = {
    {"rfc8032-1",
     "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e0652249015"
     "55fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"rfc8032-2",
     "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
     "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"rfc8032-3",
     "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
     "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
};

static void test_ed25519_vectors(void) {
    for (size_t i = 0; i < sizeof ED_VECTORS / sizeof *ED_VECTORS; i++) {
        const struct ed_vector *v = &ED_VECTORS[i];
        uint8_t seed[32], want_pub[32], want_sig[64];
        uint8_t msg[8], sk[64], pub[32], sig[64];
        size_t msg_len = strlen(v->msg) / 2;

        assert(msg_len <= sizeof msg);
        unhex(v->seed, seed, sizeof seed);
        unhex(v->pub, want_pub, sizeof want_pub);
        unhex(v->sig, want_sig, sizeof want_sig);
        unhex(v->msg, msg, msg_len);

        /* c_ekp(): key_pair() consumes (and wipes) the seed. */
        uint8_t seed_copy[32];
        memcpy(seed_copy, seed, sizeof seed);
        crypto_ed25519_key_pair(sk, pub, seed_copy);
        assert(memcmp(pub, want_pub, 32) == 0);

        /* The layout libsodium's crypto_sign_ed25519_keypair() wrote, which
         * already-bonded controllers have sitting in NVS. */
        assert(memcmp(sk, seed, 32) == 0);
        assert(memcmp(sk + 32, want_pub, 32) == 0);

        /* c_sign() is deterministic per RFC 8032, so this is byte-exact. */
        crypto_ed25519_sign(sig, sk, msg, msg_len);
        assert(memcmp(sig, want_sig, 64) == 0);

        /* c_verify(): 0 = valid. This is the direction that matters for
         * interop -- the signature bytes came from the RFC, not from us. */
        assert(crypto_ed25519_check(want_sig, want_pub, msg, msg_len) == 0);

        printf("ed25519 %s ok\n", v->name);
    }
}

static void test_ed25519_rejects_tampering(void) {
    const struct ed_vector *v = &ED_VECTORS[2];
    uint8_t pub[32], sig[64], msg[2];
    unhex(v->pub, pub, sizeof pub);
    unhex(v->sig, sig, sizeof sig);
    unhex(v->msg, msg, sizeof msg);

    /* Every byte of the signature has to matter. */
    for (size_t i = 0; i < 64; i++) {
        uint8_t bad[64];
        memcpy(bad, sig, sizeof bad);
        bad[i] = (uint8_t) (bad[i] ^ 0x01);
        assert(crypto_ed25519_check(bad, pub, msg, sizeof msg) != 0);
    }

    /* ...and so does every byte of the message and the key. */
    uint8_t bad_msg[2] = {msg[0], (uint8_t) (msg[1] ^ 0x80)};
    assert(crypto_ed25519_check(sig, pub, bad_msg, sizeof bad_msg) != 0);

    uint8_t bad_pub[32];
    memcpy(bad_pub, pub, sizeof bad_pub);
    bad_pub[7] = (uint8_t) (bad_pub[7] ^ 0x01);
    assert(crypto_ed25519_check(sig, bad_pub, msg, sizeof msg) != 0);

    /* A truncated message must not verify under the full message's signature. */
    assert(crypto_ed25519_check(sig, pub, msg, 1) != 0);

    printf("ed25519 tamper rejection ok\n");
}

/* RFC 7748 s6.1. */
static void test_x25519_vectors(void) {
    uint8_t a_priv[32], a_pub[32], b_priv[32], b_pub[32], want_shared[32];
    uint8_t got_pub[32], k_ab[32], k_ba[32];

    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
          a_priv, 32);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
          a_pub, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
          b_priv, 32);
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
          b_pub, 32);
    unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
          want_shared, 32);

    /* c_xkp(): clamping is the implementation's job (sl2_crypto.h says so),
     * and these RFC private keys are deliberately unclamped on the wire. */
    crypto_x25519_public_key(got_pub, a_priv);
    assert(memcmp(got_pub, a_pub, 32) == 0);
    crypto_x25519_public_key(got_pub, b_priv);
    assert(memcmp(got_pub, b_pub, 32) == 0);

    /* c_xsh(), both directions -- the dial computes one side with libsodium. */
    crypto_x25519(k_ab, a_priv, b_pub);
    crypto_x25519(k_ba, b_priv, a_pub);
    assert(memcmp(k_ab, want_shared, 32) == 0);
    assert(memcmp(k_ba, want_shared, 32) == 0);

    printf("x25519 rfc7748 ok\n");
}

/* libsodium's crypto_scalarmult() returned -1 on a degenerate result;
 * Monocypher returns void, so c_xsh() checks for the all-zero secret itself.
 * This pins the property that check relies on. */
static void test_x25519_small_order_is_zero(void) {
    static const uint8_t SMALL_ORDER[][32] = {
        {0},                                            /* u = 0 */
        {1},                                            /* u = 1 */
        {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae,  /* order 8 */
         0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
         0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd,
         0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00},
    };
    uint8_t priv[32], out[32];
    memset(priv, 0x42, sizeof priv);

    for (size_t i = 0; i < sizeof SMALL_ORDER / sizeof *SMALL_ORDER; i++) {
        crypto_x25519(out, priv, SMALL_ORDER[i]);
        uint8_t acc = 0;
        for (size_t j = 0; j < 32; j++)
            acc = (uint8_t) (acc | out[j]);
        assert(acc == 0);                   /* c_xsh() returns -1 here */
    }

    printf("x25519 small-order detection ok\n");
}

int main(void) {
    test_ed25519_vectors();
    test_ed25519_rejects_tampering();
    test_x25519_vectors();
    test_x25519_small_order_is_zero();
    printf("test_crypto_vectors: ALL OK\n");
    return 0;
}
