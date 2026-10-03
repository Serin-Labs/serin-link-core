# Upgrading to protocol v5

Protocol v5 changes pairing confirmation and protects room-source selections
and Link sensor reports with the controller's boot epoch. The controller and
Link firmware need coordinated updates.

## Update order and compatibility

For an existing installation, update every paired Link first, then update its
controller. Keep the existing bonds during this update. Ordinary heat-pump
controls continue to work with existing bonds. New pairing or re-pairing
requires v5 support at both ends; there is no legacy pairing fallback.

Once a bond requires epoch checks, a v5 controller rejects old Link firmware's
room-source changes and sensor reports because they do not carry the required
epoch. Update the Link to restore these functions. An existing legacy bond that
has never enabled epoch checking retains its legacy behavior until it upgrades;
that compatibility mode does not provide cross-boot replay protection. New v5
bonds require epoch checks from the moment they are saved.

A controller restart invalidates queued room-source selections. If a selection
made during a restart does not take effect, wait for current controller
information and choose the source again. Live sensor reporting resumes after
the Link learns the new epoch. The epoch protects against cross-boot replay,
subject to the existing 16-bit random token's collision risk; it does not prevent
replay within the same boot.

## Pairing and storage failures

Each end verifies a key-specific confirmation before reporting pairing success.
The controller saves the candidate bond before sending its acknowledgment.
A failed bond save reports `storage-error`; a failed re-pair restores the prior
bond. Failed forget operations retain the previous bonds and log the failure.
The C APIs `sl2_link_forget_dial` and `sl2_link_forget_all` return `false` when
the requested durable removal did not complete. Callers should check the result.

These transactions rely on the storage port's durability contract. They cannot
guarantee recovery from physically damaged flash or a storage driver that
reports failure after committing a different value.

## Selected room sources

Changing the selected Link no longer carries the previous Link's sensor health,
temperature, humidity or publish timing into the new selection. A selected Link
without its own valid reading is unavailable. Its first fresh reading is
published even when it equals the previous Link's temperature.

Successfully forgetting the selected Link returns the room source to Heat pump
and emits the existing `0` reset through `on_room_temperature`. This works when
`link_sensor:` is enabled without a Home Assistant source dropdown. Forgetting
an earlier bond slot keeps a different selected Link's MAC identity. Failed
forget operations preserve the selection.

Packet layouts and receiver rules are specified in
[the wire specification](serin-link-wire-spec.md).
