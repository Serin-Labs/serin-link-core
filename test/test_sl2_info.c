/* Host tests: INFO TLV builder helpers (sl2_info.h) — encoding bytes,
 * bounds behavior, n/a sentinels, Mitsubishi string→code tables. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "serin_link/sl2_info.h"

/* Iterate and return the (single) TLV in buf, asserting type t. */
static const uint8_t *expect_tlv(const uint8_t *buf, size_t len,
                                 uint8_t want_t, uint8_t want_l) {
    size_t off = 0;
    uint8_t t, l;
    const uint8_t *v;
    assert(sl2_tlv_next(buf, len, &off, &t, &l, &v));
    assert(t == want_t);
    assert(l == want_l);
    assert(off == len);                     /* exactly one TLV */
    return v;
}

static void test_wifi_info(void) {
    uint8_t buf[64];
    size_t off = 0;
    assert(sl2_info_put_wifi(buf, sizeof buf, &off, -61, 6, "MyNet", "10.0.0.7"));
    /* rssi, channel, "MyNet\0", "10.0.0.7\0" -> l = 2 + 6 + 9 = 17 */
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_WIFI_INFO, 17);
    assert((int8_t)v[0] == -61);
    assert(v[1] == 6);
    assert(memcmp(v + 2, "MyNet\0" "10.0.0.7\0", 15) == 0);
}

static void test_outside_t_le(void) {
    uint8_t buf[8];
    size_t off = 0;
    assert(sl2_info_put_outside_t(buf, sizeof buf, &off, -53)); /* -5.3 C */
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_OUTSIDE_T, 2);
    assert(v[0] == 0xCB && v[1] == 0xFF);   /* -53 little-endian */
}

static void test_compressor(void) {
    uint8_t buf[8];
    size_t off = 0;
    assert(sl2_info_put_compressor(buf, sizeof buf, &off, 42, 3, 1, 2));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_COMPRESSOR, 4);
    assert(v[0] == 42 && v[1] == 3 && v[2] == 1 && v[3] == 2);
}

static void test_batt(void) {
    uint8_t buf[4];
    size_t off = 0;
    assert(sl2_info_put_batt(buf, sizeof buf, &off, 87));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_SENSOR_BATT, 1);
    assert(v[0] == 87);
}

static void test_fw_info(void) {
    uint8_t buf[64];
    size_t off = 0;
    assert(sl2_info_put_fw(buf, sizeof buf, &off, "2026.6.5", "Jul 16 2026"));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_FW_INFO, 9 + 12);
    assert(memcmp(v, "2026.6.5\0" "Jul 16 2026\0", 21) == 0);
}

static void test_runtime(void) {
    uint8_t buf[8];
    size_t off = 0;
    assert(sl2_info_put_runtime(buf, sizeof buf, &off, 0x01020304u));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_RUNTIME, 4);
    assert(v[0] == 0x04 && v[1] == 0x03 && v[2] == 0x02 && v[3] == 0x01);
}

static void test_sys(void) {
    uint8_t buf[16];
    size_t off = 0;
    assert(sl2_info_put_sys(buf, sizeof buf, &off, 3600, 4));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_SYS, 5);
    assert(v[0] == 0x10 && v[1] == 0x0E && v[2] == 0 && v[3] == 0); /* 3600 LE */
    assert(v[4] == 4);
}

static void test_energy(void) {
    uint8_t buf[16];
    size_t off = 0;
    /* 450 W, 12345678 Wh */
    assert(sl2_info_put_energy(buf, sizeof buf, &off, 450, 12345678u));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_ENERGY, 6);
    assert(v[0] == 0xC2 && v[1] == 0x01);                    /* 450 LE */
    assert(v[2] == 0x4E && v[3] == 0x61 && v[4] == 0xBC && v[5] == 0x00);

    /* n/a sentinels survive round-trip */
    off = 0;
    assert(sl2_info_put_energy(buf, sizeof buf, &off, SL2_INFO_W_NA, SL2_INFO_WH_NA));
    v = expect_tlv(buf, off, SL2_TLV_ENERGY, 6);
    assert(v[0] == 0xFF && v[1] == 0xFF);
    assert(v[2] == 0xFF && v[3] == 0xFF && v[4] == 0xFF && v[5] == 0xFF);
}

static void test_bounds_whole_tlv_or_nothing(void) {
    uint8_t buf[8];                          /* too small for WIFI_INFO */
    size_t off = 0;
    assert(!sl2_info_put_wifi(buf, sizeof buf, &off, -61, 6, "MyNet", "10.0.0.7"));
    assert(off == 0);                        /* nothing written */
    /* a small TLV still fits after a failed big one */
    assert(sl2_info_put_batt(buf, sizeof buf, &off, 50));
    assert(off == 3);
}

static void test_string_tables(void) {
    assert(sl2_info_stage_code("IDLE") == 0);
    assert(sl2_info_stage_code("DIFFUSE") == 6);
    assert(sl2_info_stage_code("medium") == 3);       /* case-insensitive */
    assert(sl2_info_stage_code("MODERATE") == 4);
    assert(sl2_info_stage_code("bogus") == 0);        /* unknown -> floor */
    assert(sl2_info_stage_code("") == 0);
    assert(sl2_info_stage_code(NULL) == 0);
    assert(sl2_info_stage_code("LO") == 0);           /* prefix is not a match */
    assert(sl2_info_stage_code("GENTLE") == 2);
    assert(sl2_info_stage_code("HIGH") == 5);

    assert(sl2_info_sub_mode_code("NORMAL") == 0);
    assert(sl2_info_sub_mode_code("Defrost") == 1);
    assert(sl2_info_sub_mode_code("PREHEAT") == 2);
    assert(sl2_info_sub_mode_code("STANDBY") == 3);
    assert(sl2_info_sub_mode_code(NULL) == 0);

    assert(sl2_info_auto_sub_code("AUTO_OFF") == 0);
    assert(sl2_info_auto_sub_code("AUTO_COOL") == 1);
    assert(sl2_info_auto_sub_code("AUTO_HEAT") == 2);
    assert(sl2_info_auto_sub_code("auto_leader") == 3);
    assert(sl2_info_auto_sub_code("AUTO") == 0);
}

static void test_full_packet_stream(void) {
    /* Everything a maxed-out controller sends fits one INFO packet with
     * plenty of headroom (spec: ~120 B of 246 B payload space). */
    uint8_t buf[SL2_MTU - 4];
    size_t off = 0;
    assert(sl2_info_put_wifi(buf, sizeof buf, &off, -61, 11,
                             "a-thirty-two-character-ssid-abcd",
                             "192.168.100.200"));
    assert(sl2_info_put_outside_t(buf, sizeof buf, &off, 215));
    assert(sl2_info_put_compressor(buf, sizeof buf, &off, 42, 3, 1, 2));
    assert(sl2_info_put_batt(buf, sizeof buf, &off, 87));
    assert(sl2_info_put_fw(buf, sizeof buf, &off, "2026.6.5",
                           "Jul 16 2026, 10:00:00"));
    assert(sl2_info_put_runtime(buf, sizeof buf, &off, 12000));
    assert(sl2_info_put_sys(buf, sizeof buf, &off, 86400, 1));
    assert(sl2_info_put_energy(buf, sizeof buf, &off, 450, 12345678u));
    assert(off < 160);

    size_t it = 0, n = 0;
    uint8_t t, l;
    const uint8_t *v;
    uint8_t seen[9] = {0};
    while (sl2_tlv_next(buf, off, &it, &t, &l, &v)) {
        assert(t >= 0x01 && t <= 0x09);
        seen[t - 1] = 1;
        n++;
    }
    assert(n == 8);
    assert(!seen[SL2_TLV_HOMEKIT - 1]);      /* 0x02 never emitted here */
}

int main(void) {
    test_wifi_info();
    test_outside_t_le();
    test_compressor();
    test_batt();
    test_fw_info();
    test_runtime();
    test_sys();
    test_energy();
    test_bounds_whole_tlv_or_nothing();
    test_string_tables();
    test_full_packet_stream();
    printf("test_sl2_info: all tests passed\n");
    return 0;
}
