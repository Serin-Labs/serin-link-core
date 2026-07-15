# Security Policy

Serin Link carries real crypto (Ed25519-signed pairing, TOFU key pinning,
LMK-encrypted transport). If you believe you've found a vulnerability — in
the protocol design or in this implementation — please email
**akifbayram@gmail.com** rather than opening a public issue. You'll get a
response within a week.

The threat model, including the deliberately accepted first-contact TOFU
window, is documented in `docs/serin-link-wire-spec.md` §3. Reports that
assume a stronger model than the documented one are still welcome — worst
case, we document the gap explicitly.
