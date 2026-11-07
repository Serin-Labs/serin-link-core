# ESPHome transport recovery

This guide describes the `serin_link` adapter's recovery behavior and the
diagnostics available when a Link loses communication with its controller.
For configuration instructions, see the [README](../README.md).

## Startup and retry limits

The adapter checks ESP-NOW initialization, PMK setup, receive-callback
registration, Wi-Fi power-save enforcement, and saved-peer installation before
reporting startup complete. Setup has five attempts, separated by intervals
of 1, 2, 4, and 8 seconds. After the fifth failure, retries resume on a
subsequent Wi-Fi station start or connection event. Failures are logged.

## Periodic checks

Every five seconds, the adapter checks driver state and bonded peers. It
restores a missing peer or an incorrect interface, channel, encryption setting,
or LMK using the existing bond. Wi-Fi start and connection events also refresh
the receive callback.

A Wi-Fi connection event disables power saving immediately, because ESPHome
can reapply its `power_save_mode` when the station restarts. Modem sleep can
cause ESP-NOW frames to be missed. While connected, the adapter checks the
power-save setting once per second, including while pairing defers peer
repairs. Set `power_save_mode: NONE` in the `wifi:` block to keep the radio
awake from startup, as the CN105 package and bench configurations do.

Repairs use the same retry limit as startup, preserve the protocol session and
saved bonds, and leave correctly configured peers unchanged. Peer repairs wait
while pairing is active to avoid overwriting a candidate key. A failed
candidate installation immediately restores the original peer and closes
pairing as `cancelled`. A failed broadcast-peer installation leaves pairing
`idle`.

## Recovery limits

A silent Link alone does not trigger a transport reset. The adapter does not
call `esp_now_deinit()`, change the Wi-Fi channel, erase bonds, or restart the
controller. All encrypted peers use channel 0, following the station's channel.

These checks repair detectable software state faults. They cannot identify or
resolve every radio or driver stall. A MAC acknowledgement does not establish
that an encrypted packet reached the application. See
[Espressif's ESP-NOW documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/api-reference/network/esp_now.html).

## Diagnostic logging

Enable `DEBUG` logging for `serin_link` when investigating reconnection:

```yaml
logger:
  logs:
    serin_link: DEBUG
```

The log records Wi-Fi lifecycle events, disconnect reasons, and repair attempts.
While a bonded Link is offline, it also reports the following at most once
every 30 seconds:

- Time since the last probe. A value of `-1` means no probe has arrived since startup.
- Driver-peer lookup result and whether the peer configuration matches the bond.
- Received frame count and receive-queue drops.
- Send failures and the current radio channel.

Keys and Wi-Fi credentials are not included in these diagnostics. After the
retry limit is reached, the adapter waits for another Wi-Fi start or connection
event.

## Hardware verification

After updating the component, disconnect and reconnect the router while
leaving the Link powered on. Capture the logs throughout the test and verify
that fresh Link data returns without pairing again.

Host fault-injection tests verify the recovery paths. They do not establish
the cause of a particular hardware outage or replace testing on the device.
