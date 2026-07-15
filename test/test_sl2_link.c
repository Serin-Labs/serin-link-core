/* Host tests for the controller-role core: fake port (captured sends, fake
 * clock, in-memory kv) + deterministic toy crypto. The toy curves are
 * intentionally trivial — these tests pin the FSM, pinning, fan-out and
 * cadence logic, not the math (real curves are exercised on-device via
 * libsodium). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "serin_link/sl2_link.h"

/* ── fake port ────────────────────────────────────────────────────────── */

#define MAX_SENDS 64
typedef struct { uint8_t mac[6]; uint8_t data[250]; size_t len; } sent_t;
typedef struct { uint8_t mac[6]; uint8_t lmk[16]; bool encrypt; bool present; } fpeer_t;
typedef struct { char key[16]; uint8_t val[256]; size_t len; bool present; } fkv_t;

static struct {
    uint32_t now;
    sent_t sent[MAX_SENDS];
    int n_sent;
    fpeer_t peers[8];
    fkv_t kv[8];
    uint8_t own[6];
} F;

static void f_reset(void) { memset(&F, 0, sizeof F); memset(F.own, 0xC0, 6); }

static bool f_send(void *c, const uint8_t mac[6], const void *buf, size_t len) {
    (void)c;
    if (F.n_sent >= MAX_SENDS || len > 250) return false;
    sent_t *s = &F.sent[F.n_sent++];
    memcpy(s->mac, mac, 6);
    memcpy(s->data, buf, len);
    s->len = len;
    return true;
}
static fpeer_t *f_find_peer(const uint8_t mac[6]) {
    for (int i = 0; i < 8; i++)
        if (F.peers[i].present && memcmp(F.peers[i].mac, mac, 6) == 0)
            return &F.peers[i];
    return NULL;
}
static bool f_peer_add(void *c, const uint8_t mac[6], const uint8_t lmk[16], bool enc) {
    (void)c;
    fpeer_t *p = f_find_peer(mac);
    if (!p) {
        for (int i = 0; i < 8 && !p; i++) if (!F.peers[i].present) p = &F.peers[i];
        if (!p) return false;
    }
    p->present = true;
    memcpy(p->mac, mac, 6);
    if (lmk) memcpy(p->lmk, lmk, 16); else memset(p->lmk, 0, 16);
    p->encrypt = enc;
    return true;
}
static void f_peer_del(void *c, const uint8_t mac[6]) {
    (void)c;
    fpeer_t *p = f_find_peer(mac);
    if (p) p->present = false;
}
static bool f_own_mac(void *c, uint8_t out[6]) { (void)c; memcpy(out, F.own, 6); return true; }
static uint8_t f_channel(void *c) { (void)c; return 6; }
static uint32_t f_now(void *c) { (void)c; return F.now; }
static bool f_kv_get(void *c, const char *k, void *buf, size_t *len) {
    (void)c;
    for (int i = 0; i < 8; i++) {
        if (!F.kv[i].present || strcmp(F.kv[i].key, k) != 0) continue;
        size_t n = F.kv[i].len < *len ? F.kv[i].len : *len;
        memcpy(buf, F.kv[i].val, n);
        *len = F.kv[i].len;
        return true;
    }
    return false;
}
static bool f_kv_set(void *c, const char *k, const void *buf, size_t len) {
    (void)c;
    if (len > 256) return false;
    fkv_t *slot = NULL;
    for (int i = 0; i < 8; i++)
        if (F.kv[i].present && strcmp(F.kv[i].key, k) == 0) slot = &F.kv[i];
    for (int i = 0; i < 8 && !slot; i++) if (!F.kv[i].present) slot = &F.kv[i];
    assert(slot);
    slot->present = true;
    snprintf(slot->key, sizeof slot->key, "%s", k);
    memcpy(slot->val, buf, len);
    slot->len = len;
    return true;
}
static const sl2_port_t FPORT = {
    .ctx = NULL, .send = f_send, .peer_add = f_peer_add, .peer_del = f_peer_del,
    .own_mac = f_own_mac, .get_channel = f_channel, .now_ms = f_now, .kv_get = f_kv_get, .kv_set = f_kv_set,
    .log = NULL,
};

/* ── toy crypto (deterministic, invertible — FSM tests only) ──────────── */

static uint8_t t_ctr = 1;
static int t_rand(void *c, uint8_t *b, size_t n) {
    (void)c;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(t_ctr + i);
    t_ctr += 7;
    return 0;
}
static int t_xkp(void *c, uint8_t priv[32], uint8_t pub[32]) {
    t_rand(c, priv, 32);
    for (int i = 0; i < 32; i++) pub[i] = priv[i] ^ 0xAA;
    return 0;
}
/* shared(a_priv, B_pub) = A_pub ^ B_pub — symmetric by construction */
static int t_xsh(void *c, const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
    (void)c;
    for (int i = 0; i < 32; i++) out[i] = (priv[i] ^ 0xAA) ^ peer[i];
    return 0;
}
static int t_ekp(void *c, uint8_t priv[64], uint8_t pub[32]) {
    t_rand(c, priv, 32);                      /* seed */
    for (int i = 0; i < 32; i++) pub[i] = priv[i] ^ 0x55;
    memcpy(priv + 32, pub, 32);               /* libsodium layout: seed||pub */
    return 0;
}
static void t_sig_of(const uint8_t seed[32], const uint8_t *m, size_t ml, uint8_t sig[64]) {
    for (int i = 0; i < 64; i++)
        sig[i] = (uint8_t)(seed[i % 32] ^ m[i % ml] ^ (uint8_t)ml ^ i);
}
static int t_sign(void *c, const uint8_t priv[64], const uint8_t *m, size_t ml, uint8_t sig[64]) {
    (void)c;
    t_sig_of(priv, m, ml, sig);
    return 0;
}
static int t_verify(void *c, const uint8_t pub[32], const uint8_t *m, size_t ml, const uint8_t sig[64]) {
    (void)c;
    uint8_t seed[32], want[64];
    for (int i = 0; i < 32; i++) seed[i] = pub[i] ^ 0x55;   /* toy: invertible */
    t_sig_of(seed, m, ml, want);
    return memcmp(want, sig, 64) == 0 ? 0 : -1;
}
static const sl2_crypto_t FCRYPTO = {
    .ctx = NULL, .rand_bytes = t_rand, .x25519_keypair = t_xkp,
    .x25519_shared = t_xsh,
    .ed25519_keypair = t_ekp, .ed25519_sign = t_sign, .ed25519_verify = t_verify,
};

/* ── fake HVAC adapter ────────────────────────────────────────────────── */

static sl2_hvac_state_t H;
static uint16_t last_apply_mask;
static struct sl2_cmd_pkt last_apply_cmd;
static int n_applies;
static bool h_get_state(void *c, sl2_hvac_state_t *out) { (void)c; *out = H; return true; }
static bool h_apply(void *c, uint16_t mask, const struct sl2_cmd_pkt *cmd) {
    (void)c;
    last_apply_mask = mask;
    last_apply_cmd = *cmd;
    n_applies++;
    if (mask & SL2_CM_TEMP) H.set_dc = cmd->set_dc;   /* echo-visible change */
    if (mask & SL2_CM_MODE) H.mode = cmd->mode;
    return true;
}
static bool h_get_caps(void *c, struct sl2_caps_pkt *out) {
    (void)c;
    out->caps_flags = SL2_CF_HUM_CTRL;
    out->modes = (1u << SL2_MODE_OFF) | (1u << SL2_MODE_HEAT) | (1u << SL2_MODE_COOL);
    out->fan_steps = 5;
    out->fan_flags = SL2_FAN_HAS_AUTO;
    out->vane_v = SL2_VANECAP(5, true, true);
    out->set_min_dc = 160; out->set_max_dc = 305; out->set_step_dc = 5;
    out->ftab_id = 1;
    out->features = SL2_FEAT_OUTSIDE_T;
    snprintf(out->name, sizeof out->name, "Bench");
    return true;
}
static size_t h_tlvs(void *c, uint8_t *buf, size_t cap) {
    (void)c;
    size_t off = 0;
    int16_t oat = -12;
    sl2_tlv_put(buf, cap, &off, SL2_TLV_OUTSIDE_T, &oat, 2);
    return off;
}
static bool h_creds(void *c, char ssid[33], char psk[65]) {
    (void)c;
    snprintf(ssid, 33, "HomeNet");
    snprintf(psk, 65, "hunter22");
    return true;
}
static int n_wifi_setups;
static bool h_wifi_setup(void *c) { (void)c; n_wifi_setups++; return true; }
static const sl2_hvac_iface_t FHVAC = {
    .ctx = NULL, .get_state = h_get_state, .apply = h_apply,
    .get_caps = h_get_caps, .fill_info_tlvs = h_tlvs, .wifi_creds = h_creds,
    .wifi_setup = h_wifi_setup,
};

/* ── dial-side simulation helpers ─────────────────────────────────────── */

typedef struct {
    uint8_t mac[6];
    uint8_t id_priv[64], id_pub[32];
    uint8_t eph_priv[32], eph_pub[32];
    uint8_t lmk[16];
} fdial_t;

static void dial_make(fdial_t *d, uint8_t tag) {
    memset(d, 0, sizeof *d);
    memset(d->mac, tag, 6);
    t_ekp(NULL, d->id_priv, d->id_pub);
    t_xkp(NULL, d->eph_priv, d->eph_pub);
}

static void dial_req(const fdial_t *d, struct sl2_pair_req_pkt *req) {
    memset(req, 0, sizeof *req);
    req->type = SL2_PKT_PAIR_REQ;
    req->version = SL2_PROTO_VERSION;
    memcpy(req->src_mac, d->mac, 6);
    memcpy(req->eph_pub, d->eph_pub, 32);
    memcpy(req->id_pub, d->id_pub, 32);
    uint8_t tr[SL2_REQ_TRANSCRIPT_LEN];
    sl2_pair_req_transcript(req, tr);
    t_sign(NULL, d->id_priv, tr, sizeof tr, req->sig);
}

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* find the most recent send of a given packet type; -1 if none */
static int last_send_of(uint8_t type) {
    for (int i = F.n_sent - 1; i >= 0; i--)
        if (F.sent[i].len >= 1 && F.sent[i].data[0] == type) return i;
    return -1;
}
static int count_sends_of(uint8_t type, const uint8_t *mac) {
    int n = 0;
    for (int i = 0; i < F.n_sent; i++)
        if (F.sent[i].len >= 1 && F.sent[i].data[0] == type &&
            (!mac || memcmp(F.sent[i].mac, mac, 6) == 0)) n++;
    return n;
}

static void dial_probe(sl2_link_t *l, const fdial_t *d, uint8_t want) {
    struct sl2_probe_pkt p = { SL2_PKT_PROBE, SL2_PROTO_VERSION, want, {0} };
    sl2_link_on_recv(l, d->mac, F.own, (const uint8_t *)&p, (int)sizeof p);
}

/* run the full pairing handshake against the core; asserts success */
static void pair_dial(sl2_link_t *l, fdial_t *d) {
    int sends_before = F.n_sent;
    sl2_link_pair_start(l, 60000);
    assert(sl2_link_pairing(l));

    struct sl2_pair_req_pkt req;
    dial_req(d, &req);
    sl2_link_on_recv(l, d->mac, BCAST, (const uint8_t *)&req, (int)sizeof req);

    int ri = last_send_of(SL2_PKT_PAIR_RESP);
    assert(ri >= sends_before);
    struct sl2_pair_resp_pkt resp;
    sl2_decode_pkt(&resp, sizeof resp, F.sent[ri].data, (int)F.sent[ri].len);

    assert(resp.channel == 6);          /* controller advertises its channel */
    /* dial verifies the RESP signature + binding */
    uint8_t rt[SL2_RESP_TRANSCRIPT_LEN];
    sl2_pair_resp_transcript(&resp, d->eph_pub, rt);
    assert(t_verify(NULL, resp.id_pub, rt, sizeof rt, resp.sig) == 0);

    /* dial derives its LMK; must equal the one the core gave the radio */
    assert(sl2_derive_lmk(&FCRYPTO, d->eph_priv, resp.eph_pub,
                          d->eph_pub, resp.eph_pub, d->lmk) == 0);
    fpeer_t *p = f_find_peer(d->mac);
    assert(p && p->encrypt && memcmp(p->lmk, d->lmk, 16) == 0);

    /* confirming encrypted probe commits the bond */
    dial_probe(l, d, 0);
    assert(!sl2_link_pairing(l));
    assert(strcmp(sl2_link_pair_result(l), "paired") == 0);
}

/* ── tests ────────────────────────────────────────────────────────────── */

static void fresh(sl2_link_t *l) {
    f_reset();
    memset(&H, 0, sizeof H);
    H.hvac_link = true;
    H.mode = SL2_MODE_HEAT;
    H.action = SL2_ACT_HEATING;
    H.room_dc = 210; H.set_dc = 220;
    H.set_low_dc = SL2_DC_NA; H.set_high_dc = SL2_DC_NA;
    H.room_hum_pct = 40; H.hum_set_pct = SL2_HUM_NA;
    n_applies = 0;
    F.now = 1000;
    sl2_link_init(l, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(l));
}

static void test_identity_persists(void) {
    sl2_link_t l;
    fresh(&l);
    uint8_t id1[32]; memcpy(id1, l.id_pub, 32);
    /* second boot, same kv: identity must be identical */
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(memcmp(id1, l2.id_pub, 32) == 0);
    printf("identity persists ok\n");
}

static void test_pair_and_reboot(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD1);
    pair_dial(&l, &d);
    assert(sl2_link_dial_count(&l) == 1);

    /* reboot: bond reloads from kv, encrypted peer reinstalled */
    memset(F.peers, 0, sizeof F.peers);
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(sl2_link_dial_count(&l2) == 1);
    fpeer_t *p = f_find_peer(d.mac);
    assert(p && p->encrypt && memcmp(p->lmk, d.lmk, 16) == 0);
    printf("pair + reboot ok\n");
}

static void test_pin_mismatch(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD1);
    pair_dial(&l, &d);

    /* same MAC, new identity key: must be refused, no RESP sent */
    fdial_t evil;
    dial_make(&evil, 0xD1);            /* same mac, different keys */
    int resp_before = count_sends_of(SL2_PKT_PAIR_RESP, NULL);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&evil, &req);
    sl2_link_on_recv(&l, evil.mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    assert(count_sends_of(SL2_PKT_PAIR_RESP, NULL) == resp_before);
    assert(strcmp(sl2_link_pair_result(&l), "pin-mismatch") == 0);
    sl2_link_pair_cancel(&l);

    /* the ORIGINAL dial re-pairing (same pinned key) is allowed */
    t_xkp(NULL, d.eph_priv, d.eph_pub);   /* fresh attempt, fresh ephemeral */
    pair_dial(&l, &d);
    assert(sl2_link_dial_count(&l) == 1);  /* refreshed, not duplicated */
    printf("pin mismatch ok\n");
}

static void test_bad_signature_ignored(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD2);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d, &req);
    req.sig[0] ^= 0xFF;
    int resp_before = count_sends_of(SL2_PKT_PAIR_RESP, NULL);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    assert(count_sends_of(SL2_PKT_PAIR_RESP, NULL) == resp_before);
    assert(strcmp(sl2_link_pair_result(&l), "listening") == 0);
    sl2_link_pair_cancel(&l);
    printf("bad signature ok\n");
}

static void test_full_table(void) {
    sl2_link_t l;
    fresh(&l);
    for (int i = 0; i < SL2_MAX_DIALS; i++) {
        fdial_t d;
        dial_make(&d, (uint8_t)(0xA0 + i));
        pair_dial(&l, &d);
    }
    assert(sl2_link_dial_count(&l) == SL2_MAX_DIALS);
    sl2_link_pair_start(&l, 60000);
    assert(!sl2_link_pairing(&l));
    assert(strcmp(sl2_link_pair_result(&l), "full") == 0);
    printf("full table ok\n");
}

static void test_pair_start_mid_handshake_is_harmless(void) {
    /* Regression (2026-07-10 dry run): a second pair_start (double button
     * press / on_boot + button) regenerated the ephemeral and reset
     * CONFIRM -> WINDOW, so the dial's confirming probe never committed and
     * pairing timed out. pair_start during pairing must only extend. */
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xC7);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d, &req);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    assert(strcmp(sl2_link_pair_result(&l), "confirming") == 0);
    uint8_t eph_before[32];
    memcpy(eph_before, l.eph_pub, 32);

    F.now += 6000;
    sl2_link_pair_start(&l, 60000);            /* the second button press */
    assert(memcmp(l.eph_pub, eph_before, 32) == 0);   /* eph NOT regenerated */
    assert(strcmp(sl2_link_pair_result(&l), "confirming") == 0);

    /* dial's confirming probe still commits */
    int ri = last_send_of(SL2_PKT_PAIR_RESP);
    struct sl2_pair_resp_pkt resp;
    sl2_decode_pkt(&resp, sizeof resp, F.sent[ri].data, (int)F.sent[ri].len);
    assert(sl2_derive_lmk(&FCRYPTO, d.eph_priv, resp.eph_pub,
                          d.eph_pub, resp.eph_pub, d.lmk) == 0);
    dial_probe(&l, &d, 0);
    assert(strcmp(sl2_link_pair_result(&l), "paired") == 0);
    assert(sl2_link_dial_count(&l) == 1);
    printf("pair_start mid-handshake ok\n");
}

static void test_confirm_timeout_restores_old_lmk(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD3);
    pair_dial(&l, &d);
    uint8_t old_lmk[16];
    memcpy(old_lmk, f_find_peer(d.mac)->lmk, 16);

    /* re-pair attempt that never confirms */
    t_xkp(NULL, d.eph_priv, d.eph_pub);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d, &req);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    assert(memcmp(f_find_peer(d.mac)->lmk, old_lmk, 16) != 0);  /* re-keyed */
    F.now += SL2_PAIR_CONFIRM_MS + 1;
    sl2_link_loop(&l);
    assert(!sl2_link_pairing(&l));
    assert(strcmp(sl2_link_pair_result(&l), "timeout") == 0);
    /* radio peer restored to the still-valid bonded LMK */
    fpeer_t *p = f_find_peer(d.mac);
    assert(p && memcmp(p->lmk, old_lmk, 16) == 0);
    printf("confirm timeout restore ok\n");
}

static void test_bcast_peer_released(void) {
    /* The broadcast peer is only for the pairing handshake; every exit path
     * (timeout, cancel, commit, confirm-timeout) must return the radio slot. */
    sl2_link_t l;
    fresh(&l);

    /* window timeout */
    sl2_link_pair_start(&l, 60000);
    assert(f_find_peer(BCAST));
    F.now += 60001;
    sl2_link_loop(&l);
    assert(!sl2_link_pairing(&l) && !f_find_peer(BCAST));

    /* cancel */
    sl2_link_pair_start(&l, 60000);
    assert(f_find_peer(BCAST));
    sl2_link_pair_cancel(&l);
    assert(!f_find_peer(BCAST));

    /* successful pair: bcast gone, bonded encrypted peer kept */
    fdial_t d;
    dial_make(&d, 0xDD);
    pair_dial(&l, &d);
    assert(!f_find_peer(BCAST));
    assert(f_find_peer(d.mac) && f_find_peer(d.mac)->encrypt);

    /* confirm-phase timeout */
    fdial_t d2;
    dial_make(&d2, 0xDE);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d2, &req);
    sl2_link_on_recv(&l, d2.mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    assert(strcmp(sl2_link_pair_result(&l), "confirming") == 0);
    F.now += SL2_PAIR_CONFIRM_MS + 1;
    sl2_link_loop(&l);
    assert(!f_find_peer(BCAST));
    printf("bcast peer released ok\n");
}

static void test_state_cadence(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD4);
    pair_dial(&l, &d);
    F.n_sent = 0;

    /* first probe after pairing -> first-live STATE */
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 1);
    int si = last_send_of(SL2_PKT_STATE);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert(st.mode == SL2_MODE_HEAT && st.action == SL2_ACT_HEATING);
    assert(st.room_dc == 210 && st.set_dc == 220);
    assert(st.set_low_dc == SL2_DC_NA && st.hum_set_pct == SL2_HUM_NA);
    assert((st.flags & SL2_SF_HVAC_LINK) != 0);

    /* no change, inside heartbeat -> no resend */
    F.now += 1000;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 1);

    /* change -> resend after min interval */
    H.room_dc = 216;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 2);

    /* heartbeat fires with no change (keep probes coming) */
    for (int k = 0; k < 11; k++) { F.now += 1000; dial_probe(&l, &d, 0); sl2_link_loop(&l); }
    assert(count_sends_of(SL2_PKT_STATE, d.mac) >= 3);

    /* dial goes quiet -> offline -> nothing sent */
    F.n_sent = 0;
    F.now += SL2_DIAL_LIVE_MS + 1;
    H.room_dc = 230;
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 0);
    printf("state cadence ok\n");
}

/* SL2_WANT_STATE: a dial that lacks a fresh STATE (zone switch / resync)
 * pulls one instead of waiting out the 10 s heartbeat. The controller cannot
 * tell an "activated" dial from a background-keepalive one otherwise — the
 * probes are identical and background probes keep was_live true, which
 * suppresses the first-contact STATE. */
static void test_state_pull(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xDA);
    pair_dial(&l, &d);

    /* dial live + settled: first-live STATE out of the way, no changes */
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    F.n_sent = 0;

    /* plain keepalive probe mid-heartbeat: no STATE (pre-existing behavior) */
    F.now += 3000;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 0);

    /* WANT_STATE probe mid-heartbeat: STATE served promptly */
    F.now += 1000;
    dial_probe(&l, &d, SL2_WANT_STATE);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 1);

    /* served once per request: further loop passes don't re-send */
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 1);

    /* rate floor: a pull inside SL2_STATE_MIN_INTERVAL_MS of the last
     * STATE tx waits it out (loss-retry cadence is the dial's 1 s probe) */
    dial_probe(&l, &d, SL2_WANT_STATE);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 2);
    F.now += 100;                              /* < min interval since tx */
    dial_probe(&l, &d, SL2_WANT_STATE);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 2);
    F.now += SL2_STATE_MIN_INTERVAL_MS;        /* floor passed, want persists */
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 3);

    /* unknown spare bits: ignored, no send, no crash */
    F.now += 1000;
    dial_probe(&l, &d, (uint8_t)(1u << 7));
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == 3);
    printf("state pull ok\n");
}

static void test_cmd_apply_and_echo_all(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d1, d2;
    dial_make(&d1, 0xD5);
    pair_dial(&l, &d1);
    dial_make(&d2, 0xD6);
    pair_dial(&l, &d2);

    /* both live */
    dial_probe(&l, &d1, 0);
    dial_probe(&l, &d2, 0);
    sl2_link_loop(&l);
    F.n_sent = 0;

    struct sl2_cmd_pkt c;
    memset(&c, 0, sizeof c);
    c.type = SL2_PKT_CMD;
    c.version = SL2_PROTO_VERSION;
    c.mask = SL2_CM_TEMP | SL2_CM_MODE;
    c.mode = SL2_MODE_COOL;
    c.set_dc = 245;
    sl2_link_on_recv(&l, d1.mac, F.own, (const uint8_t *)&c, (int)sizeof c);

    assert(n_applies == 1);
    assert(last_apply_mask == (SL2_CM_TEMP | SL2_CM_MODE));
    assert(last_apply_cmd.set_dc == 245 && last_apply_cmd.mode == SL2_MODE_COOL);
    /* echo went to BOTH dials, reflecting the applied values */
    assert(count_sends_of(SL2_PKT_STATE, d1.mac) == 1);
    assert(count_sends_of(SL2_PKT_STATE, d2.mac) == 1);
    int si = last_send_of(SL2_PKT_STATE);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert(st.set_dc == 245 && st.mode == SL2_MODE_COOL);
    printf("cmd apply + echo all ok\n");
}

static void test_caps_pull_and_seq(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD7);
    pair_dial(&l, &d);
    F.n_sent = 0;

    dial_probe(&l, &d, SL2_WANT_CAPS);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_CAPS, d.mac) == 1);
    int ci = last_send_of(SL2_PKT_CAPS);
    struct sl2_caps_pkt caps;
    sl2_decode_pkt(&caps, sizeof caps, F.sent[ci].data, (int)F.sent[ci].len);
    assert(caps.caps_seq == 0);
    assert(caps.fan_steps == 5 && caps.ftab_id == 1);
    assert(sl2_vanecap_npos(caps.vane_v) == 5 && sl2_vanecap_has_swing(caps.vane_v));
    assert(strcmp(caps.name, "Bench") == 0);

    /* throttle: immediate second want doesn't resend */
    F.now += 100;
    dial_probe(&l, &d, SL2_WANT_CAPS);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_CAPS, d.mac) == 1);

    /* caps change bumps seq: visible in STATE and the next CAPS */
    sl2_link_caps_changed(&l);
    F.now += SL2_PULL_THROTTLE_MS + 1;
    dial_probe(&l, &d, SL2_WANT_CAPS);
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_STATE);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert(st.caps_seq == 1);
    ci = last_send_of(SL2_PKT_CAPS);
    sl2_decode_pkt(&caps, sizeof caps, F.sent[ci].data, (int)F.sent[ci].len);
    assert(caps.caps_seq == 1);

    /* seq survives reboot */
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(l2.caps_seq == 1);
    printf("caps pull + seq ok\n");
}

static void test_info_tlvs(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD8);
    pair_dial(&l, &d);
    F.n_sent = 0;
    dial_probe(&l, &d, SL2_WANT_INFO);
    sl2_link_loop(&l);
    int ii = last_send_of(SL2_PKT_INFO);
    assert(ii >= 0);
    const sent_t *s = &F.sent[ii];
    assert(s->len == SL2_INFO_HDR_LEN + 4);
    size_t off = 0; uint8_t t, ln; const uint8_t *v;
    assert(sl2_tlv_next(s->data + SL2_INFO_HDR_LEN, s->len - SL2_INFO_HDR_LEN,
                        &off, &t, &ln, &v));
    assert(t == SL2_TLV_OUTSIDE_T && ln == 2);
    int16_t oat; memcpy(&oat, v, 2);
    assert(oat == -12);
    printf("info tlvs ok\n");
}

static void test_wifi_req(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD9);
    pair_dial(&l, &d);
    F.n_sent = 0;
    dial_probe(&l, &d, 0);
    struct sl2_wifi_req_pkt r = { SL2_PKT_WIFI_REQ, SL2_PROTO_VERSION, {0, 0} };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&r, (int)sizeof r);
    sl2_link_loop(&l);
    int wi = last_send_of(SL2_PKT_WIFI_RESP);
    assert(wi >= 0);
    struct sl2_wifi_resp_pkt resp;
    sl2_decode_pkt(&resp, sizeof resp, F.sent[wi].data, (int)F.sent[wi].len);
    assert(resp.ok == 1);
    assert(strcmp(resp.ssid, "HomeNet") == 0 && strcmp(resp.psk, "hunter22") == 0);
    printf("wifi req ok\n");
}

static void test_wifi_setup(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xDC);
    pair_dial(&l, &d);
    dial_probe(&l, &d, 0);
    n_wifi_setups = 0;
    struct sl2_wifi_setup_pkt r = { SL2_PKT_WIFI_SETUP, SL2_PROTO_VERSION, 0 };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&r, (int)sizeof r);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);              /* hook fired */
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);              /* one request = one call */

    /* adapter reports the AP up -> next STATE carries SL2_SF_SETUP_AP */
    H.setup_ap = true;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    F.n_sent = 0;
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_STATE);
    assert(si >= 0);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert(st.flags & SL2_SF_SETUP_AP);
    H.setup_ap = false;

    /* NULL hook (adapter without on-demand AP) must not crash */
    sl2_hvac_iface_t hv2 = FHVAC;
    hv2.wifi_setup = NULL;
    sl2_link_t l2;
    f_reset();
    F.now = 1000;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &hv2);
    assert(sl2_link_start(&l2));
    fdial_t d2;
    dial_make(&d2, 0xDD);
    pair_dial(&l2, &d2);
    dial_probe(&l2, &d2, 0);
    sl2_link_on_recv(&l2, d2.mac, F.own, (const uint8_t *)&r, (int)sizeof r);
    sl2_link_loop(&l2);
    printf("wifi setup ok\n");
}

/* ── replay guard (epoch echo) ────────────────────────────────────────── */

static uint16_t last_state_epoch(const uint8_t mac[6]) {
    int n = count_sends_of(SL2_PKT_STATE, mac);
    assert(n > 0);
    (void)n;
    for (int i = F.n_sent - 1; i >= 0; i--) {
        if (F.sent[i].len >= 1 && F.sent[i].data[0] == SL2_PKT_STATE &&
            memcmp(F.sent[i].mac, mac, 6) == 0) {
            struct sl2_state_pkt st;
            sl2_decode_pkt(&st, sizeof st, F.sent[i].data, (int)F.sent[i].len);
            return st.epoch;
        }
    }
    return 0;
}

static void send_cmd_epoch(sl2_link_t *l, const fdial_t *d, uint16_t epoch,
                           int16_t set_dc) {
    struct sl2_cmd_pkt c;
    memset(&c, 0, sizeof c);
    c.type = SL2_PKT_CMD;
    c.version = SL2_PROTO_VERSION;
    c.mask = SL2_CM_TEMP;
    c.set_dc = set_dc;
    c.epoch = epoch;
    sl2_link_on_recv(l, d->mac, F.own, (const uint8_t *)&c, (int)sizeof c);
}

static void test_epoch_in_state(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE0);
    pair_dial(&l, &d);
    F.n_sent = 0;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    uint16_t e1 = last_state_epoch(d.mac);
    assert(e1 != 0);                       /* controller advertises an epoch */

    /* stable within one boot */
    F.now += SL2_STATE_HEARTBEAT_MS + 1;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    assert(last_state_epoch(d.mac) == e1);

    /* fresh on the next boot */
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    F.n_sent = 0;
    dial_probe(&l2, &d, 0);
    sl2_link_loop(&l2);
    assert(last_state_epoch(d.mac) != 0);
    assert(last_state_epoch(d.mac) != e1);
    printf("epoch in state ok\n");
}

static void test_epoch_latch_and_replay(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE1);
    pair_dial(&l, &d);
    F.n_sent = 0;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    uint16_t e = last_state_epoch(d.mac);

    /* legacy dial (epoch 0) accepted while unlatched */
    n_applies = 0;
    send_cmd_epoch(&l, &d, 0, 230);
    assert(n_applies == 1);

    /* correct echo: accepted AND latches enforcement */
    send_cmd_epoch(&l, &d, e, 240);
    assert(n_applies == 2);

    /* latched: zero and stale epochs now rejected */
    send_cmd_epoch(&l, &d, 0, 250);
    assert(n_applies == 2);
    send_cmd_epoch(&l, &d, (uint16_t)(e ^ 0x5A5A), 250);
    assert(n_applies == 2);

    /* a rejected CMD still resyncs the dial: STATE goes out promptly so the
     * dial learns the live epoch instead of wedging */
    F.n_sent = 0;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) >= 1);

    /* THE ATTACK: capture a valid CMD, reboot the controller (radio PN window
     * resets), replay the capture — the stale epoch must kill it. */
    struct sl2_cmd_pkt captured;
    memset(&captured, 0, sizeof captured);
    captured.type = SL2_PKT_CMD;
    captured.version = SL2_PROTO_VERSION;
    captured.mask = SL2_CM_MODE;
    captured.mode = SL2_MODE_OFF;
    captured.epoch = e;                    /* was valid pre-reboot */

    sl2_link_t l2;                         /* reboot: latch reloads from kv */
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    n_applies = 0;
    sl2_link_on_recv(&l2, d.mac, F.own, (const uint8_t *)&captured,
                     (int)sizeof captured);
    assert(n_applies == 0);                /* replay rejected */

    /* the real dial recovers with the fresh epoch */
    F.n_sent = 0;
    dial_probe(&l2, &d, 0);
    sl2_link_loop(&l2);
    uint16_t e2 = last_state_epoch(d.mac);
    send_cmd_epoch(&l2, &d, e2, 260);
    assert(n_applies == 1);
    printf("epoch latch + replay ok\n");
}

static void test_epoch_wifi_setup(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE2);
    pair_dial(&l, &d);
    F.n_sent = 0;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    uint16_t e = last_state_epoch(d.mac);

    /* correct echo on WIFI_SETUP latches too (dial fw may change networks
     * before ever sending a CMD) */
    n_wifi_setups = 0;
    struct sl2_wifi_setup_pkt w = { SL2_PKT_WIFI_SETUP, SL2_PROTO_VERSION, e };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&w, (int)sizeof w);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);

    /* latched: a replayed/stale WIFI_SETUP must not raise the hotspot */
    struct sl2_wifi_setup_pkt stale = { SL2_PKT_WIFI_SETUP, SL2_PROTO_VERSION,
                                        (uint16_t)(e ^ 0x1111) };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&stale, (int)sizeof stale);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);
    printf("epoch wifi setup ok\n");
}

static int t_rand_fail(void *c, uint8_t *b, size_t n) {
    (void)c; (void)b; (void)n;
    return -1;
}

static void test_epoch_rand_fail_fails_open(void) {
    /* rand_bytes failure: epoch stays 0, guard off — everything stays legacy,
     * and a 0 == 0 "match" must NOT latch enforcement. */
    sl2_link_t l;
    fresh(&l);                       /* identity generated with good rand */
    fdial_t d;
    dial_make(&d, 0xE4);
    pair_dial(&l, &d);

    sl2_crypto_t bad = FCRYPTO;
    bad.rand_bytes = t_rand_fail;
    sl2_link_t l2;                   /* reboot with a broken rand source */
    sl2_link_init(&l2, &FPORT, &bad, &FHVAC);
    assert(sl2_link_start(&l2));     /* still starts: fail open */
    F.n_sent = 0;
    dial_probe(&l2, &d, 0);
    sl2_link_loop(&l2);
    assert(last_state_epoch(d.mac) == 0);   /* honest: no epoch support */

    n_applies = 0;
    send_cmd_epoch(&l2, &d, 0, 233); /* zero echo of a zero epoch */
    assert(n_applies == 1);          /* accepted... */

    /* ...and did NOT latch: a later healthy boot still accepts legacy zeros */
    sl2_link_t l3;
    sl2_link_init(&l3, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l3));
    send_cmd_epoch(&l3, &d, 0, 234);
    assert(n_applies == 2);
    printf("epoch rand-fail fails open ok\n");
}

static void test_wifi_err_flag(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE3);
    pair_dial(&l, &d);
    H.wifi_err = true;
    F.n_sent = 0;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_STATE);
    assert(si >= 0);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert(st.flags & SL2_SF_WIFI_ERR);
    H.wifi_err = false;
    printf("wifi err flag ok\n");
}

static void test_forget(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d1, d2;
    dial_make(&d1, 0xDA);
    pair_dial(&l, &d1);
    dial_make(&d2, 0xDB);
    pair_dial(&l, &d2);
    assert(sl2_link_dial_count(&l) == 2);
    assert(sl2_link_forget_dial(&l, d1.mac));
    assert(sl2_link_dial_count(&l) == 1);
    assert(!f_find_peer(d1.mac));
    uint8_t mac[6];
    assert(sl2_link_dial_mac(&l, 0, mac) && memcmp(mac, d2.mac, 6) == 0);
    /* persisted: reboot sees one bond */
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(sl2_link_dial_count(&l2) == 1);
    printf("forget ok\n");
}

static void test_unbonded_and_broadcast_ignored(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d, stranger;
    dial_make(&d, 0xDC);
    pair_dial(&l, &d);
    F.n_sent = 0;
    /* stranger CMD: no apply */
    dial_make(&stranger, 0xEE);
    struct sl2_cmd_pkt c;
    memset(&c, 0, sizeof c);
    c.type = SL2_PKT_CMD; c.version = SL2_PROTO_VERSION;
    c.mask = SL2_CM_MODE; c.mode = SL2_MODE_OFF;
    int applies = n_applies;
    sl2_link_on_recv(&l, stranger.mac, F.own, (const uint8_t *)&c, (int)sizeof c);
    assert(n_applies == applies);
    /* broadcast-addressed CMD from the bonded dial: ignored (unicast only) */
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&c, (int)sizeof c);
    assert(n_applies == applies);
    printf("unbonded/broadcast ignored ok\n");
}

static void test_hvac_link_infer(void) {
    /* an explicit health source (native driver flag, YAML lambda) always wins */
    assert(sl2_hvac_link_infer(true, true, true, NAN) == true);
    assert(sl2_hvac_link_infer(true, false, true, 21.5f) == false);
    /* heuristic: entity claims a room temp but has none (NaN) -> the adapter
     * has never heard from the device -> link down */
    assert(sl2_hvac_link_infer(false, false, true, NAN) == false);
    assert(sl2_hvac_link_infer(false, false, true, 21.5f) == true);
    assert(sl2_hvac_link_infer(false, false, true, 0.0f) == true); /* 0C is a real temp */
    /* no room-temp feedback channel at all (IR blaster): nothing to monitor */
    assert(sl2_hvac_link_infer(false, false, false, NAN) == true);
    printf("hvac_link infer ok\n");
}

static void test_dial_info(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD8);
    pair_dial(&l, &d);

    /* before any DIAL_INFO: bonded, no identity */
    sl2_dial_view_t v;
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(!v.have_info);
    assert(v.model[0] == 0 && v.fw[0] == 0);
    assert(!sl2_link_dial_view(&l, 1, &v));   /* out of range */

    /* dial reports identity + its applied caps_seq */
    F.now += 500;
    struct sl2_dial_info_pkt di;
    memset(&di, 0, sizeof di);
    di.type = SL2_PKT_DIAL_INFO;
    di.version = SL2_PROTO_VERSION;
    di.caps_seq = 7;
    snprintf(di.model, sizeof di.model, "Serin Link 1.5\"");
    snprintf(di.fw, sizeof di.fw, "2.3.1");
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&di, (int)sizeof di);

    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.have_info);
    assert(strcmp(v.model, "Serin Link 1.5\"") == 0);
    assert(strcmp(v.fw, "2.3.1") == 0);
    assert(v.caps_seq == 7);
    assert(v.last_seen_ms >= 0 && v.last_seen_ms <= 500);
    assert(v.live);

    /* syncing derivation source: unit caps_seq (0) != dial's (7) */
    assert(sl2_link_caps_seq(&l) == 0);

    /* tolerant decode: a truncated DIAL_INFO (through caps_seq only) still
     * updates caps_seq/have_info; model/fw zero-filled. */
    uint8_t trunc[3] = { SL2_PKT_DIAL_INFO, SL2_PROTO_VERSION, 9 };
    sl2_link_on_recv(&l, d.mac, F.own, trunc, (int)sizeof trunc);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.caps_seq == 9);
    assert(v.have_info);
    assert(v.model[0] == 0);
    printf("dial info ok\n");
}

int main(void) {
    sl2_link_t probe_size_check;
    (void)probe_size_check;
    test_hvac_link_infer();
    test_identity_persists();
    test_pair_and_reboot();
    test_pin_mismatch();
    test_bad_signature_ignored();
    test_full_table();
    test_pair_start_mid_handshake_is_harmless();
    test_confirm_timeout_restores_old_lmk();
    test_bcast_peer_released();
    test_state_cadence();
    test_state_pull();
    test_cmd_apply_and_echo_all();
    test_caps_pull_and_seq();
    test_info_tlvs();
    test_wifi_req();
    test_wifi_setup();
    test_epoch_in_state();
    test_epoch_latch_and_replay();
    test_epoch_wifi_setup();
    test_epoch_rand_fail_fails_open();
    test_wifi_err_flag();
    test_dial_info();
    test_forget();
    test_unbonded_and_broadcast_ignored();
    printf("test_sl2_link: ALL OK\n");
    return 0;
}
