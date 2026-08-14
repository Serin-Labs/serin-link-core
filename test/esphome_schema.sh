#!/usr/bin/env bash
# Schema tests for the serin_link ESPHome component.
#
# pass_*.yaml must validate — and where a MUST_CONTAIN entry exists, the
# dumped (post-expansion) config must contain the marker, which is how the
# max_links entity generation is asserted without a compile. fail_*.yaml must
# be rejected AND the error must mention the expected phrase — a config
# rejected for the wrong reason (e.g. an unknown key) would otherwise pass.
#
# Needs the `esphome` CLI (override with ESPHOME=/path/to/esphome).
set -euo pipefail
cd "$(dirname "$0")/esphome"
ESPHOME=${ESPHOME:-esphome}

declare -A MUST_CONTAIN=(
  [pass_max_links.yaml]='Serin Link 2'          # generated row reaches slot 2
  [pass_max_links_bare.yaml]='Serin Link 3 MAC' # flat prefixed names, all 3 slots
  [pass_link_sensor_links.yaml]='Humidity'      # generated per-slot sensor rows
  [pass_link_sensor_links_bare.yaml]='Serin Link 2 Temperature'  # flat names, slot 2
  [pass_link_sensor_links_rows.yaml]='Office Temperature'        # hand-written names
)
declare -A MUST_REJECT_WITH=(
  [fail_rows_mismatch.yaml]='max_links'
  [fail_link_devices_mismatch.yaml]='link_devices'
  [fail_hvac_link_both.yaml]='hvac_link'
  # not 'max_links': the dumped config echo contains that literal, so it
  # would match even when the rejection is for an unrelated reason
  [fail_sensor_rows_mismatch.yaml]='must agree'
  # removed-key tombstones must explain themselves (phrases the config echo
  # cannot contain), not fall through to ESPHome's generic "invalid option"
  [fail_stale_after_removed.yaml]='report cadence'
  [fail_primary_link_removed.yaml]='internal: true'
  [fail_sensor_links_no_count.yaml]='max_links'
)

fails=0
for f in pass_*.yaml; do
  out=$("$ESPHOME" config "$f" 2>&1) || { echo "FAIL $f: did not validate"; echo "$out" | tail -20; fails=1; continue; }
  want="${MUST_CONTAIN[$f]:-}"
  if [[ -n "$want" ]] && ! grep -qF "$want" <<<"$out"; then
    echo "FAIL $f: validated but dumped config lacks '$want'"; fails=1; continue
  fi
  echo "PASS $f"
done
for f in fail_*.yaml; do
  if out=$("$ESPHOME" config "$f" 2>&1); then
    echo "FAIL $f: validated but should be rejected"; fails=1; continue
  fi
  want="${MUST_REJECT_WITH[$f]}"
  if ! grep -qF "$want" <<<"$out"; then
    echo "FAIL $f: rejected, but not for the expected reason ('$want')"
    echo "$out" | tail -20; fails=1; continue
  fi
  echo "PASS $f (rejected as expected)"
done
exit $fails
