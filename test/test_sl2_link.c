/* Host tests for the controller-role core: fake port (captured sends, fake
 * clock, in-memory kv) + deterministic toy crypto. The toy curves are
 * intentionally trivial — these tests pin the FSM, pinning, fan-out and
 * cadence logic, not the math (real curves are exercised on-device via
 * libsodium). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "serin_link/sl2_link.h"
#include "serin_link/sl2_pair_auth.h"

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
    bool fail_bond_writes;
    int bond_write_attempts;
    int storage_errors;
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
    if (strcmp(k, SL2_KV_BONDS) == 0) {
        F.bond_write_attempts++;
        if (F.fail_bond_writes) return false;
    }
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
static void f_log(void *c, int level, const char *msg) {
    (void)c;
    if (level == 0 && strstr(msg, "save failed")) F.storage_errors++;
}
static const sl2_port_t FPORT = {
    .ctx = NULL, .send = f_send, .peer_add = f_peer_add, .peer_del = f_peer_del,
    .own_mac = f_own_mac, .get_channel = f_channel, .now_ms = f_now, .kv_get = f_kv_get, .kv_set = f_kv_set,
    .log = f_log,
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
static bool h_wifi_setup(void *c) {
    (void)c;
    n_wifi_setups++;
    H.setup_ap = true;
    return true;
}
static int n_wifi_cancels;
static uint8_t cancel_result;
static uint8_t h_wifi_cancel(void *c) {
    (void)c;
    n_wifi_cancels++;
    if (cancel_result == SL2_WIFI_CANCEL_CLOSED) H.setup_ap = false;
    return cancel_result;
}
static uint32_t h_room_revision = 0x10203040u;
static uint64_t h_room_source = SL2_ROOM_SOURCE_INTERNAL_ID;
static uint8_t h_room_status;
static int n_room_sets;
/* Deliberately longer than SL2_ROOM_CATALOG_PAGE_MAX so the paging contract in
 * wire spec 10e is exercised: a full first page with a live cursor, then a
 * short final page terminated by SL2_ROOM_CATALOG_DONE. */
#define H_ROOM_CATALOG_N (SL2_ROOM_CATALOG_PAGE_MAX + 2)
static bool h_room_catalog(void *c, uint16_t cursor,
                           struct sl2_room_source_entry *entries, uint8_t cap,
                           uint8_t *count, uint16_t *next, uint32_t *revision) {
    (void)c;
    struct sl2_room_source_entry catalog[H_ROOM_CATALOG_N];
    memset(catalog, 0, sizeof catalog);
    catalog[0] = (struct sl2_room_source_entry){
        SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOM_KIND_INTERNAL,
        SL2_ROOM_SOURCE_F_SELECTABLE, "Heat pump" };
    catalog[1] = (struct sl2_room_source_entry){
        sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant"),
        SL2_ROOM_KIND_SENSOR, SL2_ROOM_SOURCE_F_SELECTABLE, "Home Assistant" };
    for (int i = 2; i < H_ROOM_CATALOG_N; i++) {
        const uint8_t mac[6] = { 0x02, 0, 0, 0, 0, (uint8_t)i };
        catalog[i].id = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
        catalog[i].kind = SL2_ROOM_KIND_LINK;
        catalog[i].flags = SL2_ROOM_SOURCE_F_SELECTABLE;
        snprintf(catalog[i].name, SL2_ROOM_SOURCE_NAME_LEN, "Serin Link %d", i - 1);
    }
    *revision = h_room_revision;
    *count = 0;
    while (cursor < H_ROOM_CATALOG_N && *count < cap)
        entries[(*count)++] = catalog[cursor++];
    *next = cursor < H_ROOM_CATALOG_N ? cursor : SL2_ROOM_CATALOG_DONE;
    return true;
}
static bool h_room_get(void *c, uint32_t *revision, uint64_t *source_id,
                       uint8_t *status) {
    (void)c;
    *revision = h_room_revision;
    *source_id = h_room_source;
    *status = h_room_status;
    return true;
}
#define H_ROOM_HA_ID sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant")
static uint8_t h_room_set(void *c, uint32_t revision, uint64_t source_id) {
    (void)c;
    n_room_sets++;
    if (revision != h_room_revision) return SL2_ROOM_SET_STALE_CATALOG;
    /* The retired automatic id is deliberately NOT accepted. */
    if (source_id != SL2_ROOM_SOURCE_INTERNAL_ID && source_id != H_ROOM_HA_ID)
        return SL2_ROOM_SET_BAD_SOURCE;
    h_room_source = source_id;
    return SL2_ROOM_SET_OK;
}
static const sl2_hvac_iface_t FHVAC = {
    .ctx = NULL, .get_state = h_get_state, .apply = h_apply,
    .get_caps = h_get_caps, .fill_info_tlvs = h_tlvs, .wifi_creds = h_creds,
    .wifi_setup = h_wifi_setup, .wifi_cancel = h_wifi_cancel,
    .room_catalog_page = h_room_catalog, .room_source_get = h_room_get,
    .room_source_set = h_room_set,
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

static void dial_confirm(sl2_link_t *l, const fdial_t *d) {
    struct sl2_pair_auth_pkt p;
    sl2_pair_auth_make(&p, SL2_PKT_PAIR_CONFIRM, d->lmk, d->mac, F.own);
    sl2_link_on_recv(l, d->mac, F.own, (const uint8_t *)&p, sizeof p);
}

static void dial_accept_pair_response(fdial_t *d, int sends_before) {
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

}

/* run the full pairing handshake against the core; asserts success */
static void pair_dial(sl2_link_t *l, fdial_t *d) {
    int sends_before = F.n_sent;
    sl2_link_pair_start(l, 60000);
    assert(sl2_link_pairing(l));

    struct sl2_pair_req_pkt req;
    dial_req(d, &req);
    sl2_link_on_recv(l, d->mac, BCAST, (const uint8_t *)&req, (int)sizeof req);

    dial_accept_pair_response(d, sends_before);

    /* Only the candidate-key proof commits the bond. */
    dial_confirm(l, d);
    assert(!sl2_link_pairing(l));
    assert(strcmp(sl2_link_pair_result(l), "paired") == 0);
}

/* Explicitly model a bond saved by old firmware, rather than weakening new pairing. */
static void legacy_single_bond(sl2_link_t *l) {
    assert(l->n_dials == 1);
    l->dial[0].bond.flags = 0;
    uint8_t blob[SL2_BONDS_BLOB_MAX];
    size_t len = sl2_bonds_encode(&l->dial[0].bond, 1, blob, sizeof blob);
    assert(f_kv_set(NULL, SL2_KV_BONDS, blob, len));
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
    n_wifi_setups = n_wifi_cancels = 0;
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
    n_room_sets = 0;
    h_room_revision = 0x10203040u;
    h_room_source = SL2_ROOM_SOURCE_INTERNAL_ID;
    h_room_status = 0;
    F.now = 1000;
    sl2_link_init(l, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(l));
}

static void test_old_queued_probe_cannot_confirm(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD8);
    pair_dial(&l, &d);
    uint8_t old_lmk[16];
    memcpy(old_lmk, d.lmk, sizeof old_lmk);
    /* Replay the signed request, followed by a PROBE decrypted with the old
     * LMK before the request was processed by the controller's RX queue. */
    struct sl2_pair_req_pkt req;
    dial_req(&d, &req);
    sl2_link_pair_start(&l, 60000);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, sizeof req);
    dial_probe(&l, &d, 0);
    assert(l.pair == SL2_PAIR_CONFIRM);
    assert(memcmp(l.dial[0].bond.lmk, old_lmk, sizeof old_lmk) == 0);
    sl2_link_pair_cancel(&l);
    assert(memcmp(f_find_peer(d.mac)->lmk, old_lmk, sizeof old_lmk) == 0);
    printf("queued old-key probe cannot confirm ok\n");
}

static void test_pair_authentication_and_retries(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD9);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d, &req);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, sizeof req);
    uint32_t deadline = l.confirm_deadline_ms;
    F.now += 100;
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, sizeof req);
    assert(l.confirm_deadline_ms == deadline);
    assert(count_sends_of(SL2_PKT_PAIR_RESP, NULL) == 2);
    struct sl2_pair_resp_pkt resp;
    int ri = last_send_of(SL2_PKT_PAIR_RESP);
    sl2_decode_pkt(&resp, sizeof resp, F.sent[ri].data, F.sent[ri].len);
    assert(sl2_derive_lmk(&FCRYPTO, d.eph_priv, resp.eph_pub,
                        d.eph_pub, resp.eph_pub, d.lmk) == 0);
    /* Another valid request from this MAC must not replace the candidate. */
    t_xkp(NULL, d.eph_priv, d.eph_pub);
    dial_req(&d, &req);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, sizeof req);
    assert(count_sends_of(SL2_PKT_PAIR_RESP, NULL) == 2);
    assert(memcmp(l.cand_lmk, d.lmk, 16) == 0);

    struct sl2_pair_auth_pkt proof;
    sl2_pair_auth_make(&proof, SL2_PKT_PAIR_CONFIRM, d.lmk, d.mac, F.own);
    for (int len = 2; len < SL2_PAIR_AUTH_MIN_LEN; len++) {
        sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&proof, len);
        assert(l.pair == SL2_PAIR_CONFIRM && l.n_dials == 0);
    }
    proof.tag[0] ^= 1;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&proof, sizeof proof);
    assert(l.pair == SL2_PAIR_CONFIRM && l.n_dials == 0);
    proof.tag[0] ^= 1;
    proof.version = 4;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&proof, sizeof proof);
    assert(l.pair == SL2_PAIR_CONFIRM && l.n_dials == 0);
    proof.version = SL2_PAIR_AUTH_MIN_VER;
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&proof, sizeof proof);
    assert(l.pair == SL2_PAIR_CONFIRM && l.n_dials == 0);
    dial_confirm(&l, &d);
    assert(l.pair == SL2_PAIR_OFF && l.n_dials == 1);
    struct sl2_pair_auth_pkt ack;
    sl2_pair_auth_make(&ack, SL2_PKT_PAIR_ACK, d.lmk, d.mac, F.own);
    int ai = last_send_of(SL2_PKT_PAIR_ACK);
    assert(ai >= 0 && sl2_pair_auth_matches(F.sent[ai].data, F.sent[ai].len, &ack));
    /* Lost ACK: repeated proof after commit gets the same authenticated ACK. */
    dial_confirm(&l, &d);
    assert(count_sends_of(SL2_PKT_PAIR_ACK, d.mac) == 2 && l.n_dials == 1);
    /* Replay the old proof into the next window's different candidate key. */
    sl2_link_pair_start(&l, 60000);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, sizeof req);
    dial_confirm(&l, &d);
    assert(l.pair == SL2_PAIR_CONFIRM);
    assert(count_sends_of(SL2_PKT_PAIR_ACK, d.mac) == 2);
    F.now = l.confirm_deadline_ms + 1;
    sl2_link_loop(&l);
    assert(l.pair == SL2_PAIR_OFF);
    assert(memcmp(f_find_peer(d.mac)->lmk, d.lmk, 16) == 0);
    printf("pair proof validation, retries and replay ok\n");
}

static void test_legacy_pair_request_is_refused(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xDA);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d, &req);
    req.version = 4;
    uint8_t tr[SL2_REQ_TRANSCRIPT_LEN];
    sl2_pair_req_transcript(&req, tr);
    t_sign(NULL, d.id_priv, tr, sizeof tr, req.sig);
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, sizeof req);
    assert(l.pair == SL2_PAIR_WINDOW && l.n_dials == 0);
    assert(last_send_of(SL2_PKT_PAIR_RESP) < 0);
    sl2_link_pair_cancel(&l);
    printf("legacy new pairing refused ok\n");
}

static void test_room_source_catalog_and_set(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE6);
    pair_dial(&l, &d);
    F.n_sent = 0;

    struct sl2_room_catalog_req_pkt q = {
        .type = SL2_PKT_ROOM_CATALOG_REQ, .version = SL2_PROTO_VERSION,
        .cursor = 0, .known_revision = 0,
    };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&q, sizeof q);
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_ROOM_CATALOG_RESP);
    assert(si >= 0);
    struct sl2_room_catalog_resp_pkt r;
    sl2_decode_pkt(&r, sizeof r, F.sent[si].data, (int)F.sent[si].len);
    /* First page: full, and next_cursor points at the remainder — NOT done. */
    assert(r.revision == h_room_revision);
    assert(r.count == SL2_ROOM_CATALOG_PAGE_MAX);
    assert(r.next_cursor == SL2_ROOM_CATALOG_PAGE_MAX);
    /* Only the entries actually filled are on the wire — a full 250-byte
     * struct is never sent when the page is short. */
    assert(F.sent[si].len == SL2_ROOM_CATALOG_RESP_HDR_LEN +
                             r.count * sizeof(struct sl2_room_source_entry));
    assert(F.sent[si].len <= SL2_MTU);
    assert(strcmp(r.entries[0].name, "Heat pump") == 0);
    assert(r.entries[1].id == H_ROOM_HA_ID && r.entries[1].kind == SL2_ROOM_KIND_SENSOR);

    /* Final page: short, and terminated by DONE so a staging client knows to
     * swap its visible list in (wire spec 10e). */
    q.cursor = r.next_cursor;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&q, sizeof q);
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_ROOM_CATALOG_RESP);
    sl2_decode_pkt(&r, sizeof r, F.sent[si].data, (int)F.sent[si].len);
    assert(r.count == H_ROOM_CATALOG_N - SL2_ROOM_CATALOG_PAGE_MAX);
    assert(r.next_cursor == SL2_ROOM_CATALOG_DONE);
    assert(F.sent[si].len == SL2_ROOM_CATALOG_RESP_HDR_LEN +
                             r.count * sizeof(struct sl2_room_source_entry));

    /* Below the v4 floor: both room-source types are dropped outright, no
     * reply of any kind — same discipline as the DIAL_SENSOR v2 gate. */
    F.n_sent = 0;
    q.cursor = 0;
    q.version = SL2_ROOM_CATALOG_MIN_VER - 1;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&q, sizeof q);
    sl2_link_loop(&l);
    assert(last_send_of(SL2_PKT_ROOM_CATALOG_RESP) < 0);
    q.version = SL2_PROTO_VERSION;

    /* A cursor past the end is answered, not dropped: empty and DONE. */
    q.cursor = H_ROOM_CATALOG_N + 5;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&q, sizeof q);
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_ROOM_CATALOG_RESP);
    sl2_decode_pkt(&r, sizeof r, F.sent[si].data, (int)F.sent[si].len);
    assert(r.count == 0 && r.next_cursor == SL2_ROOM_CATALOG_DONE);
    assert(F.sent[si].len == SL2_ROOM_CATALOG_RESP_HDR_LEN);

    struct sl2_room_source_set_pkt set = {
        .type = SL2_PKT_ROOM_SOURCE_SET, .version = SL2_PROTO_VERSION, .epoch = l.epoch,
        .request_id = 7, .revision = h_room_revision,
        .source_id = H_ROOM_HA_ID,
    };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, sizeof set);
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_ROOM_SOURCE_ACK);
    assert(si >= 0);
    struct sl2_room_source_ack_pkt a;
    sl2_decode_pkt(&a, sizeof a, F.sent[si].data, (int)F.sent[si].len);
    assert(a.request_id == 7 && a.result == SL2_ROOM_SET_OK);
    assert(a.revision == h_room_revision && a.source_id == H_ROOM_HA_ID);
    assert(n_room_sets == 1);

    /* Below the floor a SET must not reach the hook at all — no ack, and, more
     * importantly, no state change. */
    F.n_sent = 0;
    set.request_id = 9;
    set.version = SL2_ROOM_CATALOG_MIN_VER - 1;
    set.source_id = SL2_ROOM_SOURCE_INTERNAL_ID;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, sizeof set);
    sl2_link_loop(&l);
    assert(last_send_of(SL2_PKT_ROOM_SOURCE_ACK) < 0);
    assert(n_room_sets == 1 && h_room_source == H_ROOM_HA_ID);
    set.version = SL2_PROTO_VERSION;

    set.request_id = 8;
    set.revision--;
    set.source_id = SL2_ROOM_SOURCE_INTERNAL_ID;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, sizeof set);
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_ROOM_SOURCE_ACK);
    sl2_decode_pkt(&a, sizeof a, F.sent[si].data, (int)F.sent[si].len);
    assert(a.request_id == 8 && a.result == SL2_ROOM_SET_STALE_CATALOG);
    assert(a.source_id == H_ROOM_HA_ID);

    /* The retired automatic id: current revision, well-formed, and refused.
     * The ack still carries the authoritative (unchanged) selection. */
    set.request_id = 10;
    set.revision = h_room_revision;
    set.source_id = SL2_ROOM_SOURCE_LINK_AUTO_ID;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, sizeof set);
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_ROOM_SOURCE_ACK);
    sl2_decode_pkt(&a, sizeof a, F.sent[si].data, (int)F.sent[si].len);
    assert(a.request_id == 10 && a.result == SL2_ROOM_SET_BAD_SOURCE);
    assert(a.source_id == H_ROOM_HA_ID);
    printf("room source catalog + set ok\n");
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
    assert(l.dial[0].bond.flags & SL2_BOND_F_EPOCH);

    /* reboot: bond reloads from kv, encrypted peer reinstalled */
    memset(F.peers, 0, sizeof F.peers);
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(sl2_link_dial_count(&l2) == 1);
    assert(l2.dial[0].bond.flags & SL2_BOND_F_EPOCH);
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
    dial_make(&evil, 0xD1);
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

    /* dial's authenticated confirmation still commits */
    int ri = last_send_of(SL2_PKT_PAIR_RESP);
    struct sl2_pair_resp_pkt resp;
    sl2_decode_pkt(&resp, sizeof resp, F.sent[ri].data, (int)F.sent[ri].len);
    assert(sl2_derive_lmk(&FCRYPTO, d.eph_priv, resp.eph_pub,
                          d.eph_pub, resp.eph_pub, d.lmk) == 0);
    dial_confirm(&l, &d);
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
    c.epoch = l.epoch;
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
    struct sl2_wifi_setup_pkt r = { SL2_PKT_WIFI_SETUP, SL2_PROTO_VERSION, l.epoch };
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
    r.epoch = l2.epoch;
    sl2_link_on_recv(&l2, d2.mac, F.own, (const uint8_t *)&r, (int)sizeof r);
    sl2_link_loop(&l2);
    printf("wifi setup ok\n");
}

/* Wire fixtures intentionally exercise real RX/loop dispatch. */
static void wifi_wire(sl2_link_t *l, const fdial_t *d, uint8_t type,
                      uint32_t session) {
    uint8_t p[8] = { type, SL2_PROTO_VERSION, 0, 0, 0, 0, 0, 0 };
    memcpy(p + 2, &l->epoch, 2);
    memcpy(p + 4, &session, 4);
    sl2_link_on_recv(l, d->mac, F.own, p, sizeof p);
}

static void wifi_ack(const fdial_t *d, uint32_t session, uint8_t status) {
    int i = last_send_of(18);
    assert(i >= 0);
    assert(F.sent[i].len == 7);
    assert(sl2_mac_eq(F.sent[i].mac, d->mac));
    uint32_t echoed;
    memcpy(&echoed, F.sent[i].data + 2, 4);
    assert(echoed == session);
    assert(F.sent[i].data[6] == status);
}

static void test_wifi_cancel_before_start(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xCD);
    pair_dial(&l, &d);
    n_wifi_setups = 0;
    F.n_sent = 0;
    wifi_wire(&l, &d, 10, 100);
    wifi_wire(&l, &d, 17, 100);
    sl2_link_loop(&l);
    wifi_ack(&d, 100, 0);
    assert(n_wifi_setups == 0);
    wifi_wire(&l, &d, 10, 100); /* queued and delayed starts both suppressed */
    sl2_link_loop(&l);
    assert(n_wifi_setups == 0);
    wifi_wire(&l, &d, 17, 200); /* cancel can arrive before ANY start */
    sl2_link_loop(&l);
    wifi_ack(&d, 200, SL2_WIFI_CANCEL_CLOSED);
    wifi_wire(&l, &d, 10, 200);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 0);
    printf("wifi cancel before start ok\n");
}

static void test_wifi_cancel_poll_and_ownership(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t a, b;
    dial_make(&a, 0xCD); dial_make(&b, 0xCE);
    pair_dial(&l, &a); pair_dial(&l, &b);
    wifi_wire(&l, &a, 10, 100);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);
    cancel_result = SL2_WIFI_CANCEL_WAITING;
    wifi_wire(&l, &a, 17, 100);
    sl2_link_loop(&l);
    wifi_ack(&a, 100, SL2_WIFI_CANCEL_WAITING);
    assert(n_wifi_cancels == 1);
    wifi_wire(&l, &a, 10, 100);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
    wifi_wire(&l, &a, 17, 100);
    sl2_link_loop(&l);
    wifi_ack(&a, 100, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 2);
    wifi_wire(&l, &a, 17, 100);
    sl2_link_loop(&l);
    assert(n_wifi_cancels == 2); /* cached terminal result */
    wifi_wire(&l, &a, 10, 101);
    sl2_link_loop(&l);
    wifi_wire(&l, &a, 17, 100);
    sl2_link_loop(&l);
    wifi_ack(&a, 100, SL2_WIFI_CANCEL_STALE);
    assert(n_wifi_cancels == 2);
    wifi_wire(&l, &b, 10, 200);
    sl2_link_loop(&l);
    wifi_wire(&l, &a, 17, 101);
    sl2_link_loop(&l);
    wifi_ack(&a, 101, SL2_WIFI_CANCEL_STALE);
    assert(n_wifi_cancels == 2);
    wifi_wire(&l, &a, 10, 101); /* displaced owner's retry cannot take over */
    sl2_link_loop(&l);
    assert(n_wifi_setups == 3);
    wifi_wire(&l, &b, 17, 999); /* unseen cancel cannot close current window */
    sl2_link_loop(&l);
    wifi_ack(&b, 999, SL2_WIFI_CANCEL_STALE);
    assert(n_wifi_cancels == 2);
    wifi_wire(&l, &b, 17, 200);
    sl2_link_loop(&l);
    wifi_ack(&b, 200, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 3);
    printf("wifi cancel polling and ownership ok\n");
}

static void test_wifi_cancel_legacy_and_recovery(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xCD); pair_dial(&l, &d);
    H.setup_ap = true;
    wifi_wire(&l, &d, 17, 50);
    sl2_link_loop(&l);
    wifi_ack(&d, 50, SL2_WIFI_CANCEL_RECOVERY);
    assert(n_wifi_cancels == 0);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    struct sl2_wifi_setup_pkt legacy = {10, SL2_PROTO_VERSION, l.epoch};
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&legacy, sizeof legacy);
    wifi_wire(&l, &d, 17, 100);
    sl2_link_loop(&l);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_STALE);
    assert(n_wifi_cancels == 0);
    assert(n_wifi_setups == 2);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 2);
    sl2_hvac_iface_t unsupported = FHVAC;
    unsupported.wifi_cancel = NULL;
    l.hvac = &unsupported;
    wifi_wire(&l, &d, 17, 300);
    sl2_link_loop(&l);
    wifi_ack(&d, 300, SL2_WIFI_CANCEL_UNSUPPORTED);
    assert(n_wifi_cancels == 0);
    printf("wifi cancel legacy and recovery ok\n");
}

static void test_wifi_cancel_history_and_forget(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xCD); pair_dial(&l, &d);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    wifi_wire(&l, &d, 17, 100);
    sl2_link_loop(&l);
    /* Unknown cancels cannot evict protection for the current owner. */
    for (uint32_t i = 200; i < 210; i++) wifi_wire(&l, &d, 17, i);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);
    wifi_wire(&l, &d, 17, 100);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 1);
    /* A new bond must never inherit authority to cancel an old window. */
    wifi_wire(&l, &d, 10, 300);
    sl2_link_loop(&l);
    H.setup_ap = true;
    assert(sl2_link_forget_dial(&l, d.mac));
    pair_dial(&l, &d);
    wifi_wire(&l, &d, 17, 300);
    wifi_ack(&d, 300, SL2_WIFI_CANCEL_RECOVERY);
    assert(n_wifi_cancels == 1);
    wifi_wire(&l, &d, 10, 400);
    sl2_link_loop(&l);
    sl2_link_forget_all(&l);
    pair_dial(&l, &d);
    wifi_wire(&l, &d, 17, 400);
    wifi_ack(&d, 400, SL2_WIFI_CANCEL_RECOVERY);
    assert(n_wifi_cancels == 1);
    wifi_wire(&l, &d, 10, 500);
    sl2_link_loop(&l);
    pair_dial(&l, &d); /* replacing a bond also ends its session authority */
    wifi_wire(&l, &d, 17, 500);
    wifi_ack(&d, 500, SL2_WIFI_CANCEL_RECOVERY);
    assert(n_wifi_cancels == 1);
    printf("wifi cancel history and bond removal ok\n");
}

static void test_wifi_cancel_after_natural_closure(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xCD); pair_dial(&l, &d);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    H.setup_ap = false; /* successful setup or deadline closed AP */
    wifi_wire(&l, &d, 17, 200); /* new wizard start was lost */
    wifi_ack(&d, 200, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 0);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1); /* naturally completed token stays retired */

    wifi_wire(&l, &d, 10, 300);
    sl2_link_loop(&l);
    H.setup_ap = false;
    sl2_link_loop(&l); /* core observes normal completion */
    H.setup_ap = true; /* unrelated recovery opens afterwards */
    wifi_wire(&l, &d, 17, 300);
    wifi_ack(&d, 300, SL2_WIFI_CANCEL_RECOVERY);
    assert(H.setup_ap);
    assert(n_wifi_cancels == 0);
    printf("wifi cancellation after natural closure ok\n");
}

static void test_wifi_cancel_terminal_status_tracks_ap(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xCD); pair_dial(&l, &d);
    wifi_wire(&l, &d, 10, 100);
    sl2_link_loop(&l);
    wifi_wire(&l, &d, 17, 100);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_CLOSED);
    H.setup_ap = true; /* recovery opens after CLOSED ACK was lost */
    wifi_wire(&l, &d, 17, 100);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_RECOVERY);
    assert(n_wifi_cancels == 1);
    assert(H.setup_ap);
    H.setup_ap = false; /* reconnect closes recovery */
    wifi_wire(&l, &d, 17, 100);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 1);

    wifi_wire(&l, &d, 17, 200); /* never-started token stored only in history */
    wifi_ack(&d, 200, SL2_WIFI_CANCEL_CLOSED);
    H.setup_ap = true;
    wifi_wire(&l, &d, 17, 200);
    wifi_ack(&d, 200, SL2_WIFI_CANCEL_RECOVERY);
    H.setup_ap = false;
    wifi_wire(&l, &d, 17, 200);
    wifi_ack(&d, 200, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 1);
    printf("wifi cancellation terminal status tracks AP ok\n");
}

static void test_wifi_cancel_preserves_pending_and_waiting_owner(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xCD); pair_dial(&l, &d);
    wifi_wire(&l, &d, 10, 100); /* queued start, AP still closed */
    wifi_wire(&l, &d, 17, 200);
    wifi_ack(&d, 200, SL2_WIFI_CANCEL_STALE);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 1);
    cancel_result = SL2_WIFI_CANCEL_WAITING;
    wifi_wire(&l, &d, 17, 100);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_WAITING);
    H.setup_ap = false; /* trial verdict still needs polling */
    sl2_link_loop(&l);
    wifi_wire(&l, &d, 17, 300);
    wifi_ack(&d, 300, SL2_WIFI_CANCEL_STALE);
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
    wifi_wire(&l, &d, 17, 100);
    wifi_ack(&d, 100, SL2_WIFI_CANCEL_CLOSED);
    assert(n_wifi_cancels == 2);
    printf("wifi cancellation preserves pending and waiting ownership ok\n");
}

static void test_wifi_cancel_guards(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d, outsider;
    dial_make(&d, 0xCD); pair_dial(&l, &d);
    dial_make(&outsider, 0xCF);
    uint8_t p[8] = {17, SL2_PROTO_VERSION, 0, 0, 100, 0, 0, 0};
    memcpy(p + 2, &l.epoch, 2);
    F.n_sent = 0;
    sl2_link_on_recv(&l, outsider.mac, F.own, p, 8);
    sl2_link_on_recv(&l, d.mac, BCAST, p, 8);
    for (int n = 2; n < 8; n++) sl2_link_on_recv(&l, d.mac, F.own, p, n);
    p[2] ^= 1; /* wrong epoch rejected even BEFORE legacy epoch latch */
    sl2_link_on_recv(&l, d.mac, F.own, p, 8);
    p[2] ^= 1;
    p[1] = 0;
    sl2_link_on_recv(&l, d.mac, F.own, p, 8);
    p[1] = SL2_PROTO_VERSION;
    p[4] = 0; /* zero session invalid */
    sl2_link_on_recv(&l, d.mac, F.own, p, 8);
    sl2_link_loop(&l);
    assert(last_send_of(18) < 0);
    assert(n_wifi_cancels == 0);
    /* Partial session extension must not become a legacy start. */
    p[0] = 10; p[4] = 100;
    for (int n = 5; n < 8; n++) sl2_link_on_recv(&l, d.mac, F.own, p, n);
    p[2] ^= 1;
    sl2_link_on_recv(&l, d.mac, F.own, p, 8);
    sl2_link_loop(&l);
    assert(n_wifi_setups == 0);
    printf("wifi cancel authentication, epoch and lengths ok\n");
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
    legacy_single_bond(&l);
    F.n_sent = 0;
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    uint16_t e = last_state_epoch(d.mac);

    /* legacy dial (epoch 0) accepted while unlatched */
    n_applies = 0;
    n_wifi_setups = n_wifi_cancels = 0;
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
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
    n_wifi_setups = n_wifi_cancels = 0;
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
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
    legacy_single_bond(&l);
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
    legacy_single_bond(&l);

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
    n_wifi_setups = n_wifi_cancels = 0;
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
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

/* The ESPHome diagnostics rows are indexed by bond slot, and sl2_link_forget_dial
 * COMPACTS the table — so slot N can change identity when a different dial is
 * forgotten. test_forget() above pins that for removal of the FIRST of two via
 * sl2_link_dial_mac(); this pins the middle-of-three case and, crucially, that
 * sl2_link_dial_view() follows the shift too. The view carries RUNTIME state
 * (live, model, fw), not just the persisted bond, and the diagnostics entities
 * read the view — so "the runtime half moves with the bond" is load-bearing and
 * was previously asserted nowhere. */
static void test_forget_middle_compacts(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d1, d2, d3;
    dial_make(&d1, 0xE1);
    pair_dial(&l, &d1);
    dial_make(&d2, 0xE2);
    pair_dial(&l, &d2);
    dial_make(&d3, 0xE3);
    pair_dial(&l, &d3);
    assert(sl2_link_dial_count(&l) == 3);

    /* every dial probes, so all three carry runtime state (live, last_probe_ms) */
    dial_probe(&l, &d1, 0);
    dial_probe(&l, &d2, 0);
    dial_probe(&l, &d3, 0);

    sl2_dial_view_t v;
    assert(sl2_link_dial_view(&l, 2, &v) && memcmp(v.mac, d3.mac, 6) == 0);
    assert(v.live);

    assert(sl2_link_forget_dial(&l, d2.mac));      /* the MIDDLE one */
    assert(sl2_link_dial_count(&l) == 2);
    assert(!f_find_peer(d2.mac));

    /* d1 stays at 0; d3 shifts down into 1 with its runtime state intact */
    uint8_t mac[6];
    assert(sl2_link_dial_mac(&l, 0, mac) && memcmp(mac, d1.mac, 6) == 0);
    assert(sl2_link_dial_mac(&l, 1, mac) && memcmp(mac, d3.mac, 6) == 0);
    assert(sl2_link_dial_view(&l, 1, &v) && memcmp(v.mac, d3.mac, 6) == 0);
    assert(v.live);                    /* the runtime half moved with the bond */
    assert(v.last_seen_ms >= 0);

    /* the vacated tail slot reads empty — this is what makes an unpopulated
     * diagnostics row publish ""/off/NAN instead of a stale dial */
    assert(!sl2_link_dial_view(&l, 2, &v));
    assert(!sl2_link_dial_mac(&l, 2, mac));
    assert(!sl2_link_dial_live(&l, 2));

    /* persisted: a reboot decodes the same two bonds, in the same order */
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(sl2_link_dial_count(&l2) == 2);
    assert(sl2_link_dial_mac(&l2, 0, mac) && memcmp(mac, d1.mac, 6) == 0);
    assert(sl2_link_dial_mac(&l2, 1, mac) && memcmp(mac, d3.mac, 6) == 0);
    printf("forget middle compacts ok\n");
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

/* ── DIAL_INFO cert TLV tail (proto §10c) ─────────────────────────────── */

/* Forge a toy-crypto root signature: t_verify derives seed = pub ^ 0x55. */
static void root_sign(const uint8_t root_pub[32], const uint8_t *m, size_t ml,
                      uint8_t sig[64]) {
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = root_pub[i] ^ 0x55;
    t_sig_of(seed, m, ml, sig);
}

static sl2_dial_cert_t mk_cert(const uint8_t root_pub[32],
                               const uint8_t dev_pub[32], uint32_t serial) {
    sl2_dial_cert_t c;
    memset(&c, 0, sizeof c);
    c.fmt = SL2_DIAL_CERT_FMT;
    c.root_key_id = 1;
    c.model = 1; c.hw_rev = 1;
    c.serial = serial;
    c.issue_date = 201;
    memcpy(c.device_pub, dev_pub, 32);
    root_sign(root_pub, (const uint8_t *)&c, SL2_DIAL_CERT_SIGNED_LEN, c.sig);
    return c;
}

static void send_dial_info_cert(sl2_link_t *l, const fdial_t *d,
                                const sl2_dial_cert_t *c) {
    uint8_t buf[sizeof(struct sl2_dial_info_pkt) + 2 + SL2_DIAL_CERT_LEN];
    struct sl2_dial_info_pkt di;
    memset(&di, 0, sizeof di);
    di.type = SL2_PKT_DIAL_INFO;
    di.version = SL2_PROTO_VERSION;
    memcpy(buf, &di, sizeof di);
    size_t off = sizeof di;
    assert(sl2_tlv_put(buf, sizeof buf, &off, SL2_TLV_DIAL_CERT,
                       c, SL2_DIAL_CERT_LEN));
    sl2_link_on_recv(l, d->mac, F.own, buf, (int)off);
}

static void test_dial_cert(void) {
    uint8_t root[32];
    for (int i = 0; i < 32; i++) root[i] = (uint8_t)(0xA0 + i);

    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xC1);
    pair_dial(&l, &d);

    sl2_dial_view_t v;
    uint8_t raw[SL2_DIAL_CERT_LEN];

    /* no cert TLV yet (incl. a bare fixed-struct DIAL_INFO) */
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.cert_state == SL2_CERT_NONE);
    assert(!sl2_link_dial_cert(&l, 0, raw));

    /* cert arrives, no root configured -> PRESENT, raw bytes readable */
    sl2_dial_cert_t c = mk_cert(root, d.id_pub, 42);
    send_dial_info_cert(&l, &d, &c);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.cert_state == SL2_CERT_PRESENT);
    assert(sl2_link_dial_cert(&l, 0, raw));
    assert(memcmp(raw, &c, SL2_DIAL_CERT_LEN) == 0);

    /* root configured, resent -> OK (signature + pinned-identity binding) */
    sl2_link_set_root_pub(&l, root);
    send_dial_info_cert(&l, &d, &c);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.cert_state == SL2_CERT_OK);

    /* field tamper without re-sign -> INVALID */
    sl2_dial_cert_t t = c;
    t.serial = 43;
    send_dial_info_cert(&l, &d, &t);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.cert_state == SL2_CERT_INVALID);

    /* validly-signed cert for a DIFFERENT dial identity -> INVALID (pin) */
    fdial_t other;
    dial_make(&other, 0xC2);
    sl2_dial_cert_t oc = mk_cert(root, other.id_pub, 44);
    send_dial_info_cert(&l, &d, &oc);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.cert_state == SL2_CERT_INVALID);

    /* recovery: the genuine cert again -> OK */
    send_dial_info_cert(&l, &d, &c);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.cert_state == SL2_CERT_OK);

    /* truncated TLV tail (header promises more than present): ignored,
     * state unchanged */
    {
        uint8_t buf[sizeof(struct sl2_dial_info_pkt) + 2];
        struct sl2_dial_info_pkt di;
        memset(&di, 0, sizeof di);
        di.type = SL2_PKT_DIAL_INFO;
        di.version = SL2_PROTO_VERSION;
        memcpy(buf, &di, sizeof di);
        buf[sizeof di]     = SL2_TLV_DIAL_CERT;
        buf[sizeof di + 1] = SL2_DIAL_CERT_LEN;   /* ...but no payload */
        sl2_link_on_recv(&l, d.mac, F.own, buf, (int)sizeof buf);
        assert(sl2_link_dial_view(&l, 0, &v));
        assert(v.cert_state == SL2_CERT_OK);
    }

    /* wrong-length cert TLV -> INVALID (strict decode) */
    {
        uint8_t buf[sizeof(struct sl2_dial_info_pkt) + 2 + 64];
        struct sl2_dial_info_pkt di;
        memset(&di, 0, sizeof di);
        di.type = SL2_PKT_DIAL_INFO;
        di.version = SL2_PROTO_VERSION;
        memcpy(buf, &di, sizeof di);
        size_t off = sizeof di;
        assert(sl2_tlv_put(buf, sizeof buf, &off, SL2_TLV_DIAL_CERT, &c, 64));
        sl2_link_on_recv(&l, d.mac, F.own, buf, (int)off);
        assert(sl2_link_dial_view(&l, 0, &v));
        assert(v.cert_state == SL2_CERT_INVALID);
    }

    /* unknown TLV type in the tail is skipped; cert after it still applies */
    {
        uint8_t buf[sizeof(struct sl2_dial_info_pkt) + 2 + 3 + 2 + SL2_DIAL_CERT_LEN];
        struct sl2_dial_info_pkt di;
        memset(&di, 0, sizeof di);
        di.type = SL2_PKT_DIAL_INFO;
        di.version = SL2_PROTO_VERSION;
        memcpy(buf, &di, sizeof di);
        size_t off = sizeof di;
        static const uint8_t junk[3] = { 1, 2, 3 };
        assert(sl2_tlv_put(buf, sizeof buf, &off, 0x7E, junk, 3));
        assert(sl2_tlv_put(buf, sizeof buf, &off, SL2_TLV_DIAL_CERT,
                           &c, SL2_DIAL_CERT_LEN));
        sl2_link_on_recv(&l, d.mac, F.own, buf, (int)off);
        assert(sl2_link_dial_view(&l, 0, &v));
        assert(v.cert_state == SL2_CERT_OK);
    }

    /* forget clears the slot */
    assert(sl2_link_forget_dial(&l, d.mac));
    printf("dial cert ok\n");
}

/* ── DIAL_SENSOR dispatch ──────────────────────────────────────────────── */

static struct {
    int      calls;
    bool     last_is_edit;
    int16_t  last_temp_cc;
    uint16_t last_hum_cc;
    uint8_t  last_want_src;
    uint8_t  last_mac[6];
} s_rs;

static void h_room_sensor(void *ctx, const uint8_t src_mac[6],
                          const struct sl2_dial_sensor_pkt *p, bool is_edit) {
    (void)ctx;
    s_rs.calls++;
    s_rs.last_is_edit  = is_edit;
    s_rs.last_temp_cc  = p->temp_cc;
    s_rs.last_hum_cc   = p->hum_cc;
    s_rs.last_want_src = p->want_src;
    memcpy(s_rs.last_mac, src_mac, 6);
}

/* Same body as fresh(), but with a caller-supplied hvac iface so these tests
 * can wire in h_room_sensor without mutating the shared const FHVAC. */
static void fresh_hvac(sl2_link_t *l, const sl2_hvac_iface_t *hv) {
    f_reset();
    memset(&H, 0, sizeof H);
    H.hvac_link = true;
    H.mode = SL2_MODE_HEAT;
    H.action = SL2_ACT_HEATING;
    H.room_dc = 210; H.set_dc = 220;
    H.set_low_dc = SL2_DC_NA; H.set_high_dc = SL2_DC_NA;
    H.room_hum_pct = 40; H.hum_set_pct = SL2_HUM_NA;
    n_applies = 0;
    n_wifi_setups = n_wifi_cancels = 0;
    cancel_result = SL2_WIFI_CANCEL_CLOSED;
    F.now = 1000;
    sl2_link_init(l, &FPORT, &FCRYPTO, hv);
    assert(sl2_link_start(l));
}

static void test_dial_sensor_reading_only_is_not_an_edit(void) {
    memset(&s_rs, 0, sizeof s_rs);
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    fdial_t d;
    dial_make(&d, 0xE1);
    pair_dial(&l, &d);
    /* Model a loaded pre-v5 bond; new v5 pairings start protected. */
    l.dial[0].bond.flags = 0;

    uint8_t wire[SL2_DIAL_SENSOR_MIN_LEN] = {
        SL2_PKT_DIAL_SENSOR, 3, SL2_DSF_HAS_SENSOR,
        0xD4, 0x08,   /* temp_cc = 2260 centi-C = 22.60 C, little-endian */
        0x94, 0x11,   /* hum_cc = 4500 centi-% = 45.00 %, little-endian */
    };
    sl2_link_on_recv(&l, d.mac, F.own, wire, (int)sizeof wire);

    assert(s_rs.calls == 1);
    assert(s_rs.last_temp_cc == 2260);
    assert(s_rs.last_hum_cc == 4500);
    assert(!s_rs.last_is_edit);          /* short frame -> reading only */
    assert(sl2_mac_eq(s_rs.last_mac, d.mac));
    printf("dial_sensor reading-only ok\n");
}

static void test_dial_sensor_noedit_sentinel_is_not_an_edit(void) {
    memset(&s_rs, 0, sizeof s_rs);
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    fdial_t d;
    dial_make(&d, 0xE2);
    pair_dial(&l, &d);

    struct sl2_dial_sensor_pkt p = {
        .type = SL2_PKT_DIAL_SENSOR, .version = SL2_PROTO_VERSION, .epoch = l.epoch,
        .flags = SL2_DSF_HAS_SENSOR, .temp_cc = 2150, .hum_cc = 4000,
        .want_src = SL2_ROOMSRC_NOEDIT,
    };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&p, (int)sizeof p);

    assert(s_rs.calls == 1);
    assert(!s_rs.last_is_edit);
    printf("dial_sensor noedit sentinel ok\n");
}

static void test_dial_sensor_edit_is_flagged(void) {
    memset(&s_rs, 0, sizeof s_rs);
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    fdial_t d;
    dial_make(&d, 0xE3);
    pair_dial(&l, &d);

    struct sl2_dial_sensor_pkt p = {
        .type = SL2_PKT_DIAL_SENSOR, .version = SL2_PROTO_VERSION, .epoch = l.epoch,
        .flags = SL2_DSF_HAS_SENSOR, .temp_cc = 2150, .hum_cc = 4000,
        .want_src = SL2_ROOMSRC_LINK,
    };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&p, (int)sizeof p);

    assert(s_rs.calls == 1);
    assert(s_rs.last_is_edit);
    assert(s_rs.last_want_src == SL2_ROOMSRC_LINK);
    printf("dial_sensor edit flagged ok\n");
}

static void test_dial_sensor_from_unbonded_mac_is_dropped(void) {
    memset(&s_rs, 0, sizeof s_rs);
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    fdial_t d;
    dial_make(&d, 0xE4);
    pair_dial(&l, &d);

    const uint8_t stranger[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    struct sl2_dial_sensor_pkt p = {
        .type = SL2_PKT_DIAL_SENSOR, .version = SL2_PROTO_VERSION, .epoch = l.epoch,
        .flags = SL2_DSF_HAS_SENSOR, .temp_cc = 2150, .hum_cc = 4000,
        .want_src = SL2_ROOMSRC_LINK,
    };
    sl2_link_on_recv(&l, stranger, F.own, (const uint8_t *)&p, (int)sizeof p);
    assert(s_rs.calls == 0);   /* dial_by_mac() gate, same as CMD */
    printf("dial_sensor unbonded dropped ok\n");
}

/* v3 rescaled temp/hum from deci to centi WITHOUT changing the packet size,
 * so a stale v2 frame decodes cleanly into the wrong units. Nothing else in
 * the stack rejects on version (peer version is only a skew warning), so
 * the hook-never-fires assertion below is the only proof the guard runs. */
static void test_dial_sensor_v2_frame_is_rejected(void) {
    memset(&s_rs, 0, sizeof s_rs);
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    fdial_t d;
    dial_make(&d, 0xE6);
    pair_dial(&l, &d);

    uint8_t frame[9] = {0};
    frame[0] = SL2_PKT_DIAL_SENSOR;
    frame[1] = 2;                     /* v2 -- must be dropped */
    frame[2] = SL2_DSF_HAS_SENSOR;
    frame[3] = 0xC7; frame[4] = 0x00; /* 199 in whatever unit */
    sl2_link_on_recv(&l, d.mac, F.own, frame, (int)sizeof frame);

    assert(s_rs.calls == 0);
    printf("dial_sensor v2 frame rejected ok\n");
}

static void test_dial_sensor_v3_frame_is_accepted(void) {
    memset(&s_rs, 0, sizeof s_rs);
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    fdial_t d;
    dial_make(&d, 0xE7);
    pair_dial(&l, &d);
    /* Model a loaded pre-v5 bond; new v5 pairings start protected. */
    l.dial[0].bond.flags = 0;

    uint8_t frame[9] = {0};
    frame[0] = SL2_PKT_DIAL_SENSOR;
    frame[1] = 3;
    frame[2] = SL2_DSF_HAS_SENSOR;
    frame[3] = 0xC7; frame[4] = 0x09; /* 2503 centi-C = 25.03 C */
    frame[5] = 0x10; frame[6] = 0x17; /* 5904 centi-% = 59.04 % */
    frame[7] = SL2_ROOMSRC_NOEDIT;
    sl2_link_on_recv(&l, d.mac, F.own, frame, (int)sizeof frame);

    assert(s_rs.calls == 1);
    assert(s_rs.last_temp_cc == 2503);
    assert(s_rs.last_hum_cc == 5904);
    printf("dial_sensor v3 frame accepted ok\n");
}

static void test_dial_sensor_null_hook_is_safe(void) {
    sl2_link_t l;
    fresh(&l);                /* shared FHVAC: room_sensor is NULL */
    fdial_t d;
    dial_make(&d, 0xE5);
    pair_dial(&l, &d);

    struct sl2_dial_sensor_pkt p = {
        .type = SL2_PKT_DIAL_SENSOR, .version = SL2_PROTO_VERSION, .epoch = l.epoch,
        .flags = SL2_DSF_HAS_SENSOR, .temp_cc = 2150, .hum_cc = 4000,
        .want_src = SL2_ROOMSRC_NOEDIT,
    };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&p, (int)sizeof p);
    /* no crash = pass */
    printf("dial_sensor null hook safe ok\n");
}

/* ── remote screen gate ───────────────────────────────────────────────── */

/* The HA presence switch travels as STATE.flags2 bits. The shared change
 * detection (memcmp on the built packet) must treat a gate flip as a change,
 * fanning it out inside SL2_STATE_MIN_INTERVAL_MS rather than waiting for
 * the 10 s heartbeat — a motion-triggered wake that lands seconds late is
 * the feature not working. */
static void test_screen_gate_in_state(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE4);
    pair_dial(&l, &d);
    F.n_sent = 0;

    /* no gate configured -> neither bit (legacy zero-fill semantics) */
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_STATE);
    assert(si >= 0);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert((st.flags2 & (SL2_SF2_SCREEN_CTL | SL2_SF2_SCREEN_OFF)) == 0);

    /* switch exists and says ON -> CTL set, OFF clear, sent as a change */
    H.screen_ctl = true;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_STATE);
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert((st.flags2 & SL2_SF2_SCREEN_CTL) != 0);
    assert((st.flags2 & SL2_SF2_SCREEN_OFF) == 0);

    /* room empties -> both bits */
    int sends_before = count_sends_of(SL2_PKT_STATE, d.mac);
    H.screen_off = true;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == sends_before + 1);
    si = last_send_of(SL2_PKT_STATE);
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert((st.flags2 & SL2_SF2_SCREEN_CTL) != 0);
    assert((st.flags2 & SL2_SF2_SCREEN_OFF) != 0);
    printf("screen gate in state ok\n");
}

/* ── sun-down night gate ─────────────────────────────────────────────── */

/* The sun gate travels as STATE.flags2 bits, same two-bit scheme as the
 * screen gate (zero-fill legacy sender = not declared). A flip must fan out
 * as a change inside SL2_STATE_MIN_INTERVAL_MS — sunset landing 10 s late is
 * fine, but the change-detection contract is what this pins. */
static void test_night_gate_in_state(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE5);
    pair_dial(&l, &d);
    F.n_sent = 0;

    /* no gate computed -> neither bit (legacy zero-fill semantics) */
    dial_probe(&l, &d, 0);
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_STATE);
    assert(si >= 0);
    struct sl2_state_pkt st;
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert((st.flags2 & (SL2_SF2_NIGHT_CTL | SL2_SF2_NIGHT)) == 0);

    /* gate computed, sun up -> CTL set, NIGHT clear */
    H.night_ctl = true;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_STATE);
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert((st.flags2 & SL2_SF2_NIGHT_CTL) != 0);
    assert((st.flags2 & SL2_SF2_NIGHT) == 0);

    /* sun sets -> both bits, sent as a change */
    int sends_before = count_sends_of(SL2_PKT_STATE, d.mac);
    H.night = true;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    assert(count_sends_of(SL2_PKT_STATE, d.mac) == sends_before + 1);
    si = last_send_of(SL2_PKT_STATE);
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert((st.flags2 & SL2_SF2_NIGHT_CTL) != 0);
    assert((st.flags2 & SL2_SF2_NIGHT) != 0);
    assert(st.night_ceil == 0);   /* no opinion declared -> zero on the wire */

    /* ceiling opinion rides the claimed reserved byte, change-detected */
    H.night_ceil_pct = 35;
    F.now += SL2_STATE_MIN_INTERVAL_MS + 1;
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_STATE);
    sl2_decode_pkt(&st, sizeof st, F.sent[si].data, (int)F.sent[si].len);
    assert(st.night_ceil == 35);
    printf("night gate in state ok\n");
}

/* The dial reports its actual screen state in DIAL_SENSOR flags; the core
 * stashes it in the per-dial runtime slot and surfaces it through dial_view,
 * so adapters publish a status entity from the existing poll — no new
 * receive path. The stash must happen even with no room_sensor hook wired
 * (FHVAC has none): screen status is core state, not adapter policy. */
static void test_dial_screen_status_view(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xE5);
    pair_dial(&l, &d);

    sl2_dial_view_t v;
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(!v.screen_valid);                  /* nothing reported yet */

    struct sl2_dial_sensor_pkt p = {
        .type = SL2_PKT_DIAL_SENSOR, .version = SL2_PROTO_VERSION, .epoch = l.epoch,
        .flags = SL2_DSF_SCREEN_VALID | SL2_DSF_SCREEN_ON,
        .temp_cc = SL2_CC_NA, .hum_cc = SL2_HUM_CC_NA,
        .want_src = SL2_ROOMSRC_NOEDIT,
    };
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&p, (int)sizeof p);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.screen_valid && v.screen_on);

    p.flags = SL2_DSF_SCREEN_VALID;           /* screen went dark */
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&p, (int)sizeof p);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(v.screen_valid && !v.screen_on);

    /* a dial that stops reporting (legacy flags) reads unknown, never off */
    p.flags = SL2_DSF_HAS_SENSOR;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&p, (int)sizeof p);
    assert(sl2_link_dial_view(&l, 0, &v));
    assert(!v.screen_valid);
    printf("dial screen status view ok\n");
}


/* Storage failures must preserve the table in RAM, on the radio and at reboot. */
static void storage_reboot(sl2_link_t *l) {
    memset(F.peers, 0, sizeof F.peers);
    F.n_sent = 0;
    sl2_link_init(l, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(l));
}

static void storage_begin_pair(sl2_link_t *l, fdial_t *d) {
    int sends_before = F.n_sent;
    sl2_link_pair_start(l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(d, &req);
    sl2_link_on_recv(l, d->mac, BCAST, (const uint8_t *)&req, sizeof req);
    assert(l->pair == SL2_PAIR_CONFIRM);
    dial_accept_pair_response(d, sends_before);
}

static void test_pair_storage_failure(void) {
    /* Cover an empty table and an addition beside an unrelated bond. */
    for (int keep_existing = 0; keep_existing < 2; keep_existing++) {
        sl2_link_t l;
        fresh(&l);
        fdial_t keep, candidate;
        dial_make(&keep, 0xD1);
        if (keep_existing) pair_dial(&l, &keep);
        sl2_dial_rt_t original = l.dial[0];
        dial_make(&candidate, 0xD2);
        storage_begin_pair(&l, &candidate);
        int ack_before = count_sends_of(SL2_PKT_PAIR_ACK, NULL);
        F.fail_bond_writes = true;
        F.now += 100;
        dial_confirm(&l, &candidate);
        assert(strcmp(sl2_link_pair_result(&l), "storage-error") == 0);
        assert(count_sends_of(SL2_PKT_PAIR_ACK, NULL) == ack_before);
        assert(!sl2_link_pairing(&l));
        assert(F.storage_errors == 1);
        assert(l.n_dials == keep_existing);
        assert(memcmp(&l.dial[0], &original, sizeof original) == 0);
        assert(!f_find_peer(candidate.mac));
        assert((f_find_peer(keep.mac) != NULL) == (keep_existing != 0));
        assert(!f_find_peer(BCAST));
        uint8_t zeros[32] = {0};
        assert(memcmp(l.cand_lmk, zeros, sizeof l.cand_lmk) == 0);
        assert(memcmp(l.eph_priv, zeros, sizeof l.eph_priv) == 0);
        sl2_link_t rebooted;
        storage_reboot(&rebooted);
        assert(rebooted.n_dials == keep_existing);
        assert(!f_find_peer(candidate.mac));
        assert(memcmp(&rebooted.dial[0].bond, &original.bond, sizeof original.bond) == 0);
    }
    printf("pair storage failure ok\n");
}

static void test_repair_storage_failure(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d, keep;
    dial_make(&d, 0xD1);
    pair_dial(&l, &d);
    dial_make(&keep, 0xD2);
    pair_dial(&l, &keep);
    send_cmd_epoch(&l, &d, l.epoch, 230);
    assert(l.dial[0].bond.flags & SL2_BOND_F_EPOCH);
    sl2_dial_rt_t original[2];
    memcpy(original, l.dial, sizeof original);
    t_xkp(NULL, d.eph_priv, d.eph_pub);
    storage_begin_pair(&l, &d);
    assert(memcmp(f_find_peer(d.mac)->lmk, original[0].bond.lmk, 16) != 0);
    int ack_before = count_sends_of(SL2_PKT_PAIR_ACK, NULL);
    F.fail_bond_writes = true;
    F.now += 100;
    dial_confirm(&l, &d);
    assert(strcmp(sl2_link_pair_result(&l), "storage-error") == 0);
    assert(count_sends_of(SL2_PKT_PAIR_ACK, NULL) == ack_before);
    assert(l.n_dials == 2);
    assert(memcmp(l.dial, original, sizeof original) == 0);
    assert(memcmp(f_find_peer(d.mac)->lmk, original[0].bond.lmk, 16) == 0);
    sl2_link_t rebooted;
    storage_reboot(&rebooted);
    for (int i = 0; i < 2; i++)
        assert(memcmp(&rebooted.dial[i].bond, &original[i].bond, sizeof original[i].bond) == 0);
    printf("re-pair storage failure ok\n");
}

static void test_forget_storage_failure(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d[3];
    for (int i = 0; i < 3; i++) {
        dial_make(&d[i], (uint8_t)(0xD1 + i));
        pair_dial(&l, &d[i]);
    }
    sl2_dial_rt_t original[3];
    memcpy(original, l.dial, sizeof original);
    F.fail_bond_writes = true;
    assert(!sl2_link_forget_dial(&l, d[1].mac));
    assert(F.storage_errors == 1);
    assert(l.n_dials == 3);
    assert(memcmp(l.dial, original, sizeof original) == 0);
    for (int i = 0; i < 3; i++) assert(f_find_peer(d[i].mac));
    sl2_link_t rebooted;
    storage_reboot(&rebooted);
    assert(rebooted.n_dials == 3);
    F.fail_bond_writes = false;
    assert(sl2_link_forget_dial(&l, d[1].mac));
    assert(l.n_dials == 2 && !f_find_peer(d[1].mac));
    assert(memcmp(&l.dial[1], &original[2], sizeof original[2]) == 0);
    storage_reboot(&rebooted);
    assert(rebooted.n_dials == 2);
    assert(memcmp(rebooted.dial[1].bond.mac, d[2].mac, 6) == 0);
    printf("forget storage failure ok\n");
}

static void test_forget_all_storage_failure(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD1);
    pair_dial(&l, &d);
    sl2_dial_rt_t original = l.dial[0];
    F.fail_bond_writes = true;
    assert(!sl2_link_forget_all(&l));
    assert(F.storage_errors == 1);
    assert(l.n_dials == 1);
    assert(memcmp(&l.dial[0], &original, sizeof original) == 0);
    assert(f_find_peer(d.mac));
    sl2_link_t rebooted;
    storage_reboot(&rebooted);
    assert(rebooted.n_dials == 1);
    F.fail_bond_writes = false;
    assert(sl2_link_forget_all(&l));
    assert(l.n_dials == 0 && !f_find_peer(d.mac));
    storage_reboot(&rebooted);
    assert(rebooted.n_dials == 0);
    printf("forget all storage failure ok\n");
}

static void test_epoch_storage_failure(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD1);
    pair_dial(&l, &d);
    l.dial[0].bond.flags &= (uint8_t)~SL2_BOND_F_EPOCH;
    uint8_t blob[SL2_BONDS_BLOB_MAX];
    size_t len = sl2_bonds_encode(&l.dial[0].bond, 1, blob, sizeof blob);
    assert(f_kv_set(NULL, SL2_KV_BONDS, blob, len));
    storage_reboot(&l);
    F.fail_bond_writes = true;
    int attempts = F.bond_write_attempts;
    send_cmd_epoch(&l, &d, l.epoch, 230);
    assert(n_applies == 0);
    assert(!(l.dial[0].bond.flags & SL2_BOND_F_EPOCH));
    send_cmd_epoch(&l, &d, 0, 240);
    assert(n_applies == 0); /* observed echo still protects this boot */
    send_cmd_epoch(&l, &d, l.epoch, 250);
    assert(n_applies == 0 && F.bond_write_attempts == attempts + 2);
    assert(F.storage_errors == 2);
    sl2_link_t rebooted;
    storage_reboot(&rebooted);
    assert(!(rebooted.dial[0].bond.flags & SL2_BOND_F_EPOCH));
    send_cmd_epoch(&rebooted, &d, 0, 255);
    assert(n_applies == 1); /* legacy grace really remains across reboot */
    F.fail_bond_writes = false;
    send_cmd_epoch(&rebooted, &d, rebooted.epoch, 260);
    assert(n_applies == 2 && (rebooted.dial[0].bond.flags & SL2_BOND_F_EPOCH));
    storage_reboot(&rebooted);
    assert(rebooted.dial[0].bond.flags & SL2_BOND_F_EPOCH);
    send_cmd_epoch(&rebooted, &d, 0, 270);
    assert(n_applies == 2);
    printf("epoch storage failure retry ok\n");
}

static void test_room_packets_legacy_upgrade(void) {
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    memset(&s_rs, 0, sizeof s_rs);
    fdial_t d;
    dial_make(&d, 0xEC);
    pair_dial(&l, &d);
    /* Model a loaded pre-v5 bond; new v5 pairings start protected. */
    l.dial[0].bond.flags = 0;
    struct sl2_room_source_set_pkt set = {
        .type = SL2_PKT_ROOM_SOURCE_SET, .version = 4,
        .revision = h_room_revision, .source_id = SL2_ROOM_SOURCE_INTERNAL_ID,
    };
    struct sl2_dial_sensor_pkt sensor = {
        .type = SL2_PKT_DIAL_SENSOR, .version = 3,
        .want_src = SL2_ROOMSRC_LINK,
    };
    /* Unprotected legacy firmware keeps both historical write paths. */
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, 16);
    assert(l.dial[0].room_source_req);
    l.dial[0].room_source_req = false;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&sensor, 9);
    assert(s_rs.calls == 1 && s_rs.last_is_edit);
    /* Claiming v5 never enters that legacy grace window. */
    set.version = sensor.version = 5;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, sizeof set);
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&sensor, sizeof sensor);
    assert(!l.dial[0].room_source_req && s_rs.calls == 1);
    set.epoch = sensor.epoch = l.epoch;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, 17);
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&sensor, 10);
    assert(!l.dial[0].room_source_req && s_rs.calls == 1);
    /* A reading-only v5 report ratchets the bond without a CMD first. */
    sensor.want_src = SL2_ROOMSRC_NOEDIT;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&sensor, sizeof sensor);
    assert(s_rs.calls == 2 && !s_rs.last_is_edit);
    assert(l.dial[0].bond.flags & SL2_BOND_F_EPOCH);
    set.version = 4; sensor.version = 3;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, 16);
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&sensor, 9);
    assert(!l.dial[0].room_source_req && s_rs.calls == 2);
    /* RNG failure cannot reopen either room-data mutation path. */
    l.epoch = 0;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, 16);
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&sensor, 9);
    assert(!l.dial[0].room_source_req && s_rs.calls == 2);
    printf("room packets legacy upgrade ok\n");
}

/* Raw v5 tails also exercise prefix compatibility against pre-v5 builds. */
static void test_room_packets_replay_freshness(void) {
    sl2_hvac_iface_t hv = FHVAC;
    hv.room_sensor = h_room_sensor;
    sl2_link_t l;
    fresh_hvac(&l, &hv);
    n_room_sets = 0;
    memset(&s_rs, 0, sizeof s_rs);
    fdial_t d;
    dial_make(&d, 0xEB);
    pair_dial(&l, &d);
    send_cmd_epoch(&l, &d, l.epoch, 230); /* existing protected bond */
    uint8_t set[18] = {SL2_PKT_ROOM_SOURCE_SET, 5, 42, 0};
    memcpy(set + 4, &h_room_revision, 4);
    uint64_t id = SL2_ROOM_SOURCE_INTERNAL_ID;
    memcpy(set + 8, &id, 8);
    uint8_t sensor[11] = {SL2_PKT_DIAL_SENSOR, 5,
        SL2_DSF_HAS_SENSOR | SL2_DSF_SCREEN_VALID | SL2_DSF_SCREEN_ON,
        0x66, 0x08, 0xA0, 0x0F, SL2_ROOMSRC_LINK, 0};
    /* Missing, zero, stale, and partial epochs cannot mutate protected state.
     * Low-byte-only truncation must fail even when zero-fill matches epoch. */
    l.epoch = 0x34;
    const uint16_t epochs[] = {0, 0x99, 0x34, 0x34};
    const int set_lens[] = {18, 18, 16, 17};
    const int sensor_lens[] = {11, 11, 9, 10};
    for (size_t i = 0; i < sizeof epochs / sizeof epochs[0]; i++) {
        memcpy(set + 16, &epochs[i], 2);
        memcpy(sensor + 9, &epochs[i], 2);
        l.dial[0].last_probe_ms = 123;
        sl2_link_on_recv(&l, d.mac, F.own, set, set_lens[i]);
        sl2_link_on_recv(&l, d.mac, F.own, sensor, sensor_lens[i]);
        assert(!l.dial[0].room_source_req);
        assert(s_rs.calls == 0);
        assert(!l.dial[0].screen_valid);
        assert(l.dial[0].last_probe_ms == 123);
        assert(l.dial[0].pend_state);
    }
    memcpy(set + 16, &l.epoch, 2);
    memcpy(sensor + 9, &l.epoch, 2);
    sl2_link_on_recv(&l, d.mac, F.own, set, sizeof set);
    sl2_link_loop(&l);
    assert(n_room_sets == 1);
    sl2_link_on_recv(&l, d.mac, F.own, sensor, sizeof sensor);
    assert(s_rs.calls == 1 && s_rs.last_is_edit);
    /* Downgrade cannot bypass the latch. Neither legacy edits nor legacy
     * reading-only frames may refresh the last accepted measurement. */
    set[1] = 4; sensor[1] = 3;
    /* A tail on an old version cannot masquerade as v5 support either. */
    sl2_link_on_recv(&l, d.mac, F.own, set, sizeof set);
    sl2_link_on_recv(&l, d.mac, F.own, sensor, sizeof sensor);
    assert(!l.dial[0].room_source_req && s_rs.calls == 1);
    sl2_link_on_recv(&l, d.mac, F.own, set, 16);
    sl2_link_on_recv(&l, d.mac, F.own, sensor, 9);
    sensor[7] = SL2_ROOMSRC_NOEDIT;
    sl2_link_on_recv(&l, d.mac, F.own, sensor, 7);
    assert(!l.dial[0].room_source_req && s_rs.calls == 1);
    /* Reboot preserves protection despite a stable catalog revision. */
    sl2_link_t reboot;
    sl2_link_init(&reboot, &FPORT, &FCRYPTO, &hv);
    assert(sl2_link_start(&reboot));
    assert(reboot.epoch != l.epoch);
    set[1] = sensor[1] = 5;
    sl2_link_on_recv(&reboot, d.mac, F.own, set, sizeof set);
    sl2_link_on_recv(&reboot, d.mac, F.own, sensor, sizeof sensor);
    assert(!reboot.dial[0].room_source_req && s_rs.calls == 1);
    memcpy(set + 16, &reboot.epoch, 2);
    memcpy(sensor + 9, &reboot.epoch, 2);
    sl2_link_on_recv(&reboot, d.mac, F.own, set, sizeof set);
    sl2_link_loop(&reboot);
    assert(n_room_sets == 2);
    sl2_link_on_recv(&reboot, d.mac, F.own, sensor, sizeof sensor);
    assert(s_rs.calls == 2 && !s_rs.last_is_edit);
    printf("room packets replay freshness ok\n");
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (strcmp(argv[1], "pair-storage") == 0) test_pair_storage_failure();
        else if (strcmp(argv[1], "repair-storage") == 0) test_repair_storage_failure();
        else if (strcmp(argv[1], "forget-storage") == 0) test_forget_storage_failure();
        else if (strcmp(argv[1], "forget-all-storage") == 0) test_forget_all_storage_failure();
        else if (strcmp(argv[1], "epoch-storage") == 0) test_epoch_storage_failure();
        else return 2;
        return 0;
    }
    test_pair_storage_failure();
    test_repair_storage_failure();
    test_forget_storage_failure();
    test_forget_all_storage_failure();
    test_epoch_storage_failure();
    sl2_link_t probe_size_check;
    (void)probe_size_check;
    test_room_packets_legacy_upgrade();
    test_room_packets_replay_freshness();
    test_pair_authentication_and_retries();
    test_legacy_pair_request_is_refused();
    test_old_queued_probe_cannot_confirm();
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
    test_wifi_cancel_before_start();
    test_wifi_cancel_poll_and_ownership();
    test_wifi_cancel_legacy_and_recovery();
    test_wifi_cancel_guards();
    test_wifi_cancel_after_natural_closure();
    test_wifi_cancel_terminal_status_tracks_ap();
    test_wifi_cancel_preserves_pending_and_waiting_owner();
    test_wifi_cancel_history_and_forget();
    test_epoch_in_state();
    test_epoch_latch_and_replay();
    test_epoch_wifi_setup();
    test_epoch_rand_fail_fails_open();
    test_wifi_err_flag();
    test_dial_info();
    test_dial_cert();
    test_forget();
    test_forget_middle_compacts();
    test_unbonded_and_broadcast_ignored();
    test_dial_sensor_reading_only_is_not_an_edit();
    test_dial_sensor_noedit_sentinel_is_not_an_edit();
    test_dial_sensor_edit_is_flagged();
    test_dial_sensor_from_unbonded_mac_is_dropped();
    test_dial_sensor_null_hook_is_safe();
    test_dial_sensor_v2_frame_is_rejected();
    test_dial_sensor_v3_frame_is_accepted();
    test_screen_gate_in_state();
    test_night_gate_in_state();
    test_dial_screen_status_view();
    test_room_source_catalog_and_set();
    printf("test_sl2_link: ALL OK\n");
    return 0;
}
