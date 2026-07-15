# Contributing

Thanks for your interest in Serin Link!

## The one rule that will bite you: vendored copies

`esphome/components/serin_link/sl2_*.{h,c}` are **generated** flattened copies
of `include/serin_link/` + `src/`. Never edit them directly:

1. Edit the canonical files (`include/serin_link/`, `src/`).
2. Run `tools/sync_esphome.sh`.
3. Commit both.

CI fails if the vendored copies drift from the canonical tree.

## Tests

`test/run.sh` builds and runs the host suite (plain gcc, no hardware). Core
changes need a test — the suite drives the full pairing/liveness/fan-out FSM
against a fake port with deterministic toy crypto, so most protocol behavior
is host-testable. Wire-layout changes must follow the additive-growth rules
in `docs/serin-link-wire-spec.md` §1 (bump `SL2_PROTO_VERSION`, never
resize or reorder shipped fields, keep the `sizeof` static asserts true).

The ESPHome adapter (`serin_link.cpp`) is verified by compilation in CI plus
on-hardware testing.

## Licensing of contributions

The project is Apache-2.0. By submitting a contribution you agree it is
licensed under Apache-2.0 (inbound = outbound). Keep the core free of
platform dependencies — anything platform-specific belongs behind the port
layer (`sl2_port.h`, `sl2_crypto.h`).
