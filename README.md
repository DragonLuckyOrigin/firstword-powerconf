# FirstWord for Anker PowerConf

**Stop losing the first word when you start recording.**
A tiny, free Windows tool against the cold-start silence that cuts off the first words on the
**Anker PowerConf (A3301) USB speakerphone** (USB `VID_291A` / `PID_3301`).

> Not affiliated with or endorsed by Anker. "Anker" and "PowerConf" are named only to identify the device.

*Deutsche Anleitung: [weiter unten](#deutsch).*

---

## The problem

When an app starts recording from a PowerConf that no other app is using, the device
delivers **about 2 seconds of pure digital silence** first. Dictation, voice input and voice
messages started that way lose their first words:

- “*One two three, today is Wednesday*” arrives as “*…three, today is Wednesday*”
- Observed in ChatGPT, Claude and Codex voice input and the Windows Sound Recorder; because the zeros come from the device, any app that opens the microphone cold is expected to see them
- The second recording right after the first one works fine, which makes it look random

Observed on the tested devices with a native WASAPI capture (48 kHz, mono, float32):

| | Cold start (no FirstWord) | With FirstWord running |
|---|---:|---:|
| Silence before the first word | **1,942 ms** of exact zeros | **~0.4 s** (includes the speaker's reaction time) |
| First audible signal | 2,000 ms | 420 ms |

Same behaviour on two PowerConf units on two Windows 11 PCs, before and after the
November 2026 USB-audio update (KB5132061). Windows audio enhancements off or on make no difference.

## The fix

FirstWord keeps the PowerConf microphone **open in the background** (Windows shared mode).
While FirstWord holds the device, it is already awake when your app starts recording, so the
first word gets through. Right after plugging the device in (or waking the PC) FirstWord needs a
few seconds to wake it once – see the tip below.

- 🔒 **Records nothing.** Audio buffers are released immediately and unread – nothing is stored, analysed or sent. No network code.
- 🎯 **Only the PowerConf over USB.** Matched by USB hardware ID; never opens Bluetooth or any other microphone.
- 🔁 **Self-healing.** Re-opens the microphone after sleep, USB unplug/replug and reboot (tested on hardware); an audio-service restart is handled in code but not hardware-tested.
- 🪶 **Tiny.** ~10 MB RAM, no window, no service, no admin rights, starts with Windows (per-user autostart).
- 🔇 The PowerConf's own mute button still mutes while the microphone is held open (observed in an earlier warm-stream test).
- Windows shows the “microphone in use” icon permanently – that is FirstWord doing its job.

## Install (2 minutes)

1. Download **`FirstWord-win-x64.zip`** from the [latest release](https://github.com/DragonLuckyOrigin/firstword-powerconf/releases/latest) and unzip it.
2. Open the unzipped folder, click into the Explorer address bar, type `powershell`, press Enter.
3. Run:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install.ps1
   ```
   Expected output: `Installed: C:\Users\<you>\AppData\Local\Programs\FirstWord\FirstWord.exe`
4. Test: open any app, start recording, speak **immediately** – on the tested setup the first word was there (wait ~20 s after plugging the device in first).

The installer verifies the SHA-256 of `FirstWord.exe`, copies it to
`%LOCALAPPDATA%\Programs\FirstWord`, adds a per-user autostart entry
(`HKCU\...\Run\FirstWord`) and starts it. No administrator rights needed.

**Unsigned file:** `FirstWord.exe` is not code-signed. Windows SmartScreen may warn when you
download or start it (“More info → Run anyway”). The source is right here and every release
is built by GitHub Actions from this repository; `SHA256SUMS.txt` in the zip lists the hash.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File .\uninstall.ps1            # -RemoveLog also deletes the log
```
Stop without uninstalling: `& "$env:LOCALAPPDATA\Programs\FirstWord\FirstWord.exe" --stop`

## Check that it works

Log: `%LOCALAPPDATA%\FirstWord\firstword.log` (state changes only, no audio):
```
start: target USB VID_291A PID_3301
device found: {0.0.1.00000000}.{…} on USB\VID_291A&PID_3301&MI_01\…
stream open: {0.0.1.00000000}.{…}
```
`no target device present` means no active PowerConf USB microphone was matched – for example because it is not connected via USB or is disabled in the Windows sound settings.

**Tip:** right after plugging the PowerConf in, waking the PC or signing in after a reboot, give it
~20 seconds (FirstWord itself has to wake the device once; `stream open` in the log only means the
stream has started), or reopen your app if the very first recording still misses a word.

## Setup instructions for AI assistants / automated setup

Use these exact steps when an assistant (Claude, Codex, …) sets this up on a new PC:

1. Download the latest release asset `FirstWord-win-x64.zip`
   (`https://github.com/DragonLuckyOrigin/firstword-powerconf/releases/latest`) into a new, empty folder and unzip it.
2. Verify: `Get-FileHash .\FirstWord.exe` must equal the hash in `SHA256SUMS.txt`.
3. **Let the user run `install.ps1` in their own normal PowerShell.** Sandboxed or packaged agent
   shells (e.g. desktop apps installed as MSIX) may virtualize writes under `AppData`, so an install
   run from inside the agent can land in a private copy that Windows never autostarts.
4. Verify after install: process `FirstWord` is running, the log contains `stream open`, and
   `HKCU:\Software\Microsoft\Windows\CurrentVersion\Run` has the value `FirstWord`.
5. Acceptance: wait ~20 s after `stream open`, then the user closes and reopens a recording app and speaks immediately → first word present.
   Then repeat for USB unplug/replug, sleep/wake and reboot: each time wait ~20 s, then test speech. A new `stream open`
   log line appears only when FirstWord actually had to reopen the stream (a stream that survived sleep is kept).

## Other USB microphones (advanced, no guarantee)

FirstWord accepts `--vid XXXX --pid YYYY` (4 hex digits each) for another USB microphone with
the same start-up problem. The installer does not set these; add them to the `Run` value
yourself. Bluetooth devices are never opened (holding a Bluetooth mic open would force the
headset into low-quality call mode).

## Build from source

Requires Visual Studio 2022 Build Tools (C++) and the Windows SDK. `build.cmd` builds and runs the
unit tests, then builds `out\FirstWord.exe`. No third-party code. Details: [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

Exit codes: 0 ok · 2 bad arguments · 3 `--stop` without a running instance · 4 start-up error.

License: [MIT](LICENSE).

---

<a id="deutsch"></a>
## Deutsch

**FirstWord** hilft beim **Anker PowerConf (A3301)** über USB gegen das fehlende erste Wort: Beim
kalten Öffnen lieferte das Gerät in unseren Tests rund **2 Sekunden digitale Stille** – beobachtet in
ChatGPT, Claude, Codex und dem Windows-Audiorekorder. FirstWord hält das Mikrofon im Hintergrund
offen. **Es nimmt nichts auf, speichert nichts und sendet nichts.**

**Installation:**
1. `FirstWord-win-x64.zip` aus dem [neuesten Release](https://github.com/DragonLuckyOrigin/firstword-powerconf/releases/latest) laden und entpacken.
2. Im entpackten Ordner in die Explorer-Adresszeile `powershell` tippen, Enter.
3. `powershell -ExecutionPolicy Bypass -File .\install.ps1`
4. Testen: App öffnen, Aufnahme starten, **sofort** sprechen – im getesteten Aufbau war das erste Wort da (nach dem Einstecken ~20 s warten).

**Hinweise:** Die EXE ist **nicht signiert**, Windows SmartScreen kann warnen („Weitere
Informationen → Trotzdem ausführen“). Windows zeigt dauerhaft „Mikrofon wird verwendet“ – gewollt.
Nach dem Einstecken des PowerConf ~20 Sekunden warten. Entfernen mit `uninstall.ps1`.
Log: `%LOCALAPPDATA%\FirstWord\firstword.log`. Kein offizielles Anker-Produkt.
