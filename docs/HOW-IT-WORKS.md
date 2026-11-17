# How FirstWord works

## Diagnosis
A native WASAPI shared-mode capture client (Microsoft's CaptureSharedEventDriven pattern,
48 kHz mono float32) opened the PowerConf microphone cold and then warm:

- **Cold:** the first 93,234 samples (1,942 ms) are exactly `0.0`. Packets arrive from the start
  (first buffer ~35 ms after `Start`), device positions are continuous, no `SILENT` flag is set –
  the device itself sends digital zeros while it starts up.
- **Warm** (stream opened again right after): 256 zero samples (5 ms).
- Identical on a second PowerConf unit on a second PC; unchanged by Windows update KB5132061
  (usbaudio.sys 10.0.26100.9974) and by the Windows audio-enhancements switch.
- Note: in silence the PowerConf also outputs exact zeros (noise gate), so any measurement must
  include immediate speech.

Conclusion: device/firmware start-up behaviour. Keeping one capture stream open avoids it for every
other application.

## Design
- **Device match:** every active capture endpoint → its PnP node `SWD\MMDEVAPI\<endpoint id>`
  (Windows does not expose `PKEY_Device_InstanceId` on audio endpoints) → up to 4 parent nodes →
  `DEVPKEY_Device_HardwareIds`. A match needs bus `USB` and the exact tokens `VID_291A` and
  `PID_3301`. Malformed data is a non-match (fail-closed); read errors make the scan incomplete,
  and an incomplete scan never closes an existing stream.
- **Holding:** `IAudioClient::Initialize(SHARED, EVENTCALLBACK, 0, 0, mix format)`,
  `SetEventHandle`, `Start`; every event drains all packets with `GetBuffer`/`ReleaseBuffer`
  without reading them.
- **Recovery:** `IMMNotificationClient` only signals an event; the main loop
  (`WaitForMultipleObjects`, max 500 ms) rescans, drains, and runs a 3 s packet watchdog after
  every wake-up. Failed opens retry with 1 / 2 / 5 / 10 s backoff; enumerator failures rebuild the
  enumerator; healthy streams are never restarted. Max 8 streams.
- **Control:** single instance via `Local\FirstWord` mutex; `--stop` (and the scripts) signal
  `Local\FirstWord.Stop`.
- **Log:** state changes only, 256 KB with one rotation.

## Tests
`tests/unit_tests.cpp` covers hardware-ID token matching, strict multi-string parsing, endpoint
node derivation, the ancestor walk (match, top of tree, level limit, read/parent errors), target
reconciliation, backoff and enumerator recovery, and the watchdog.

Hardware acceptance on Windows 11 with a PowerConf A3301: app test, USB replug, sleep/wake and
reboot with autostart all passed.
