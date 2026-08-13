# Vendored: Monocypher 4.0.3

`monocypher.c/.h` and `monocypher-ed25519.c/.h` are copied **verbatim** from
[Monocypher](https://monocypher.org) 4.0.3
(<https://github.com/LoupVaillant/Monocypher/releases/tag/4.0.3>), `src/` and
`src/optional/`. Licence: dual 2-clause BSD / CC-0, see `LICENCE.monocypher.md`.

They back the `sl2_crypto_t` hooks in `serin_link.cpp`: Ed25519 (RFC 8032, via
the SHA-512 variant in `monocypher-ed25519`) and X25519 (RFC 7748). Nothing
else in this component uses Monocypher — HMAC-SHA256/HKDF stay pinned in-tree
in `sl2_sha256.h`, and the AEAD/Argon2/BLAKE2b half of `monocypher.c` is
dropped by `--gc-sections`.

## Why vendored rather than an IDF component

An ESPHome image can contain exactly one libsodium, and it is never ours.
`api: encryption:` pulls `esphome/noise-c`, which ESPHome converts into an IDF
component keyed `libsodium`; that entry is written after — and so overwrites —
anything `add_idf_component()` declares under the same key, and declaring a
different key fails component discovery outright on the name-without-namespace
collision. ESPHome's port is a curated subset for noise-c with no
`crypto_sign_*` and no SHA-512, so binding to whichever copy wins link-errors
on signing. Monocypher's symbols do not collide with libsodium's, so both can
sit in the same image.

## Updating

Replace the four files from an upstream release tarball and re-run
`test/run.sh` — `test/test_crypto_vectors.c` compiles these sources directly
and checks them against the RFC 8032 and RFC 7748 vectors, including the
64-byte `seed || public_key` secret-key layout that already-provisioned
identity keys in NVS depend on.

Do not "fix" warnings or reformat these files: keeping them byte-identical to
upstream is what makes the next update a straight copy. `test/run.sh` compiles
them under their own flags rather than the suite's `-Werror` for that reason.
