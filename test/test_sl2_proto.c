/* Host tests: wire layout, tolerant decode, TLV codec, display tables,
 * pairing transcripts, bond-table codec. Pure C, no hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "serin_link/sl2_proto.h"
#include "serin_link/sl2_bond.h"
#include "serin_link/sl2_sha256.h"

/* RFC 5869 vectors. The LMK derivation must be bit-exact on every adapter —
 * a platform libsodium disagreed with itself across two builds (2026-07-10),
 * so the KDF is pinned in-tree and pinned here. */
static void test_hkdf_rfc5869(void) {
    /* SHA-256 sanity: "abc" */
    uint8_t h[32];
    sl2_sha256("abc", 3, h);
    static const uint8_t abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    assert(memcmp(h, abc, 32) == 0);

    /* TC1 */
    uint8_t ikm[22], salt[13], info[10], okm[42];
    memset(ikm, 0x0b, sizeof ikm);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    sl2_hkdf_sha256(ikm, 22, salt, 13, info, 10, okm, 42);
    static const uint8_t tc1[42] = {
        0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,
        0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,
        0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65 };
    assert(memcmp(okm, tc1, 42) == 0);

    /* TC3: zero-length salt and info */
    sl2_hkdf_sha256(ikm, 22, NULL, 0, NULL, 0, okm, 42);
    static const uint8_t tc3[42] = {
        0x8d,0xa4,0xe7,0x75,0xa5,0x63,0xc1,0x8f,0x71,0x5f,0x80,0x2a,0x06,0x3c,0x5a,0x31,
        0xb8,0xa1,0x1f,0x5c,0x5e,0xe1,0x87,0x9e,0xc3,0x45,0x4e,0x5f,0x3c,0x73,0x8d,0x2d,
        0x9d,0x20,0x13,0x95,0xfa,0xa4,0xb6,0x1a,0x96,0xc8 };
    assert(memcmp(okm, tc3, 42) == 0);

    /* RFC 4231 TC6: HMAC key longer than the 64-byte block gets hashed */
    uint8_t longkey[131], mac[32];
    const char *msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    memset(longkey, 0xaa, sizeof longkey);
    sl2_hmac_sha256(longkey, sizeof longkey,
                    (const uint8_t *)msg6, strlen(msg6), mac);
    static const uint8_t tc6[32] = {
        0x60,0xe4,0x31,0x59,0x1e,0xe0,0xb6,0x7f,0x0d,0x8a,0x26,0xaa,0xcb,0xf5,0xb7,0x7f,
        0x8e,0x0b,0xc6,0x21,0x37,0x28,0xc5,0x14,0x05,0x46,0x04,0x0f,0x0e,0xe3,0x7f,0x54 };
    assert(memcmp(mac, tc6, 32) == 0);
    printf("hkdf rfc5869 ok\n");
}

static void test_layout(void) {
    /* sizes are compile-time asserted in the header; pin key offsets too so a
     * reordered field can't slip through as "same size". */
    assert(offsetof(struct sl2_state_pkt, fault) == 10);
    assert(offsetof(struct sl2_state_pkt, room_dc) == 12);
    assert(offsetof(struct sl2_state_pkt, set_low_dc) == 16);
    assert(offsetof(struct sl2_state_pkt, room_hum_pct) == 20);
    assert(offsetof(struct sl2_state_pkt, epoch) == 24);
    assert(offsetof(struct sl2_cmd_pkt, mask) == 2);
    assert(offsetof(struct sl2_cmd_pkt, set_dc) == 9);
    assert(offsetof(struct sl2_cmd_pkt, use_f) == 16);
    assert(offsetof(struct sl2_cmd_pkt, epoch) == 17);
    assert(offsetof(struct sl2_wifi_setup_pkt, epoch) == 2);
    assert(offsetof(struct sl2_caps_pkt, modes) == 4);
    assert(offsetof(struct sl2_caps_pkt, set_min_dc) == 12);
    assert(offsetof(struct sl2_caps_pkt, features) == 20);
    assert(offsetof(struct sl2_caps_pkt, name) == 22);
    assert(offsetof(struct sl2_pair_req_pkt, eph_pub) == 8);
    assert(offsetof(struct sl2_pair_req_pkt, sig) == 72);
    assert(SL2_PROTO_VERSION == 2);
    assert(offsetof(struct sl2_dial_info_pkt, caps_seq) == 2);
    assert(offsetof(struct sl2_dial_info_pkt, model) == 3);
    assert(offsetof(struct sl2_dial_info_pkt, fw) == 27);
    assert(sizeof(struct sl2_dial_info_pkt) == 43);
    printf("layout ok\n");
}

static void test_tolerant_decode(void) {
    struct sl2_state_pkt in, out;
    memset(&in, 0, sizeof in);
    in.type = SL2_PKT_STATE; in.version = SL2_PROTO_VERSION;
    in.room_dc = 215; in.set_dc = 220;
    in.set_low_dc = SL2_DC_NA; in.set_high_dc = SL2_DC_NA;
    in.room_hum_pct = 44; in.hum_set_pct = SL2_HUM_NA;

    /* longer future packet: unknown tail ignored */
    uint8_t big[sizeof in + 10];
    memcpy(big, &in, sizeof in);
    memset(big + sizeof in, 0xEE, 10);
    sl2_decode_pkt(&out, sizeof out, big, (int)sizeof big);
    assert(memcmp(&out, &in, sizeof in) == 0);

    /* shorter packet: missing tail decodes as zeros */
    sl2_decode_pkt(&out, sizeof out, &in, 14);
    assert(out.room_dc == 215);
    assert(out.set_dc == 0 && out.room_hum_pct == 0);
    printf("tolerant decode ok\n");
}

static void test_tlv(void) {
    uint8_t buf[64];
    size_t off = 0;
    int16_t oat = -53;
    uint8_t comp[4] = { 42, 3, 0x02, 0x01 };
    assert(sl2_tlv_put(buf, sizeof buf, &off, SL2_TLV_OUTSIDE_T, &oat, 2));
    assert(sl2_tlv_put(buf, sizeof buf, &off, SL2_TLV_COMPRESSOR, comp, 4));
    assert(sl2_tlv_put(buf, sizeof buf, &off, 0x90 /* vendor */, "xyz", 3));
    assert(off == 4 + 6 + 5);

    /* doesn't fit -> refused, off unchanged */
    uint8_t huge[60];
    memset(huge, 0, sizeof huge);
    size_t before = off;
    assert(!sl2_tlv_put(buf, sizeof buf, &off, 0x91, huge, 60));
    assert(off == before);

    /* iterate */
    size_t it = 0; uint8_t t, l; const uint8_t *v;
    assert(sl2_tlv_next(buf, off, &it, &t, &l, &v));
    assert(t == SL2_TLV_OUTSIDE_T && l == 2);
    int16_t got; memcpy(&got, v, 2);
    assert(got == -53);
    assert(sl2_tlv_next(buf, off, &it, &t, &l, &v));
    assert(t == SL2_TLV_COMPRESSOR && l == 4 && v[0] == 42);
    assert(sl2_tlv_next(buf, off, &it, &t, &l, &v));
    assert(t == 0x90 && l == 3 && memcmp(v, "xyz", 3) == 0);
    assert(!sl2_tlv_next(buf, off, &it, &t, &l, &v));

    /* truncated TLV never reads past len */
    uint8_t trunc[3] = { SL2_TLV_SENSOR_BATT, 5, 99 };
    it = 0;
    assert(!sl2_tlv_next(trunc, sizeof trunc, &it, &t, &l, &v));
    printf("tlv ok\n");
}

static void test_ftab(void) {
    /* the two table landmarks that motivated table-not-linear */
    assert(sl2_ftab1_f_to_c(71) == 22.0f);
    assert(sl2_ftab1_f_to_c(72) == 22.5f);
    assert(sl2_ftab1_c_to_f(22.0f) == 71);
    assert(sl2_ftab1_c_to_f(22.5f) == 72);
    /* clamps */
    assert(sl2_ftab1_f_to_c(40) == 16.0f);
    assert(sl2_ftab1_f_to_c(99) == 30.5f);
    /* every table entry round-trips */
    for (int f = SL2_FTAB1_MIN_F; f <= SL2_FTAB1_MAX_F; f++)
        assert(sl2_ftab1_c_to_f(sl2_ftab1_f_to_c(f)) == f);
    /* display dispatch: ftab 1 setpoint in range vs linear fallback */
    assert(sl2_dc_to_display(220, true, 1) == 71);
    assert(sl2_dc_to_display(220, true, 0) == 72);          /* linear 22C = 71.6F */
    assert(sl2_dc_to_display(-50, true, 1) == 23);          /* out of range -> linear */
    assert(sl2_dc_to_display(215, false, 1) == 22);
    assert(sl2_display_to_dc(72, true, 1) == 225);
    assert(sl2_display_to_dc(22, false, 1) == 220);
    printf("ftab ok\n");
}

static void test_transcripts(void) {
    struct sl2_pair_req_pkt req;
    memset(&req, 0, sizeof req);
    req.type = SL2_PKT_PAIR_REQ; req.version = SL2_PROTO_VERSION;
    memset(req.src_mac, 0x11, 6);
    memset(req.eph_pub, 0x22, 32);
    memset(req.id_pub, 0x33, 32);
    uint8_t tr[SL2_REQ_TRANSCRIPT_LEN];
    assert(sl2_pair_req_transcript(&req, tr) == 80);
    assert(memcmp(tr, "SLv2-req", 8) == 0);
    assert(tr[8] == SL2_PKT_PAIR_REQ && tr[9] == SL2_PROTO_VERSION);
    assert(tr[10] == 0x11 && tr[16] == 0x22 && tr[48] == 0x33);

    struct sl2_pair_resp_pkt resp;
    memset(&resp, 0, sizeof resp);
    resp.type = SL2_PKT_PAIR_RESP; resp.version = SL2_PROTO_VERSION;
    memset(resp.src_mac, 0x44, 6);
    memset(resp.eph_pub, 0x55, 32);
    memset(resp.id_pub, 0x66, 32);
    resp.channel = 11;
    uint8_t dial_eph[32]; memset(dial_eph, 0x22, 32);
    uint8_t rt[SL2_RESP_TRANSCRIPT_LEN];
    assert(sl2_pair_resp_transcript(&resp, dial_eph, rt) == 114);
    assert(memcmp(rt, "SLv2-resp", 9) == 0);
    assert(rt[11] == 0x44 && rt[17] == 0x55 && rt[49] == 0x66 && rt[81] == 0x22);
    assert(rt[113] == 11);
    printf("transcripts ok\n");
}

static void test_bonds(void) {
    sl2_dial_bond_t in[SL2_MAX_DIALS], out[SL2_MAX_DIALS];
    memset(in, 0, sizeof in);
    for (int i = 0; i < SL2_MAX_DIALS; i++) {
        memset(in[i].mac, i + 1, 6);
        memset(in[i].lmk, (i + 1) ^ 0xFF, 16);
        memset(in[i].id_pub, 0x80 + i, 32);
    }
    uint8_t blob[SL2_BONDS_BLOB_MAX];
    for (int n = 0; n <= SL2_MAX_DIALS; n++) {
        size_t len = sl2_bonds_encode(in, n, blob, sizeof blob);
        assert(len == 2 + (size_t)n * SL2_BOND_REC_SIZE);
        assert(sl2_bonds_decode(blob, len, out) == n);
        assert(memcmp(out, in, (size_t)n * sizeof *in) == 0);
    }
    /* malformed */
    assert(sl2_bonds_encode(in, SL2_MAX_DIALS + 1, blob, sizeof blob) == 0);
    size_t len = sl2_bonds_encode(in, 2, blob, sizeof blob);
    blob[0] = 1;                                  /* wrong fmt */
    assert(sl2_bonds_decode(blob, len, out) == -1);
    blob[0] = SL2_BOND_FMT; blob[1] = 9;          /* impossible count */
    assert(sl2_bonds_decode(blob, len, out) == -1);
    blob[1] = 2;
    assert(sl2_bonds_decode(blob, len - 1, out) == -1);   /* short */
    printf("bonds ok\n");
}

int main(void) {
    test_layout();
    test_tolerant_decode();
    test_tlv();
    test_ftab();
    test_transcripts();
    test_bonds();
    test_hkdf_rfc5869();
    printf("test_sl2_proto: ALL OK\n");
    return 0;
}
