# launcher-multilink

Download game files over several internet connections at once (LAN + Wi-Fi + USB tethering, ...)
from inside the game launcher itself. No proxy, no VPN, no server.

| Launcher | Games | Status |
|---|---|---|
| HoYoPlay 1.18.0.380 | Honkai: Star Rail, Genshin Impact, Zenless Zone Zero, Honkai Impact 3rd | Measured with Star Rail 4.7.0: 98.6 → 138 Mbit/s with three links |
| Steam (steamclient64 11.23.65.88) | Steam games, including the Steam release of Wuthering Waves | Injection and patch verified. Download speed not measured yet |

All HoYoPlay games use the same download engine (`sophon.dll`), so they get the same treatment.
Only Star Rail was measured.

## How it works

`multilink_inject.exe` loads `multilink.dll` into the launcher process. The DLL:

1. **Picks the downloader** from `multilink.ini`. The profile whose `process=` is the current exe
   names the module to patch (`sophon.dll` for HoYoPlay, `steamclient64.dll` for Steam).
2. **Patches that module's import table** for ws2_32 `bind` and `connect`. Every new outgoing TCP
   connection to a public IPv4 address gets a source address from the next adapter in the list
   (round-robin). Windows routes a socket out of the adapter that owns its source address, so the
   connections spread over all links. Loopback, LAN, IPv6 and every other module's traffic are left alone.
3. **HoYoPlay only: turns off HTTP/2.** sophon embeds Chromium's network stack. Over HTTP/2 it
   sends every chunk through *one* connection, which can't be split. The DLL watches for
   `sophon.dll` to load (`LdrRegisterDllNotification`), hooks `Sophon_InitEnv`, and rewrites
   `"enable_http2": true` to `false` in the init JSON. sophon then uses HTTP/1.1 with many
   parallel connections. The hook only installs when the function prologue matches build
   1.18.0.380; on any other build it logs that and leaves HTTP/2 alone.

This adds speed only when **each link has its own bottleneck**: different ISPs, a phone on mobile
data, or separately authenticated sessions on a network that caps each one. Wi-Fi and Ethernet
from the same router share that router's uplink and won't add anything. Check with the speed test first.

## Build

Visual Studio 2022 (MSVC, x64, C++20). No other dependencies.

```bat
build.bat
```

Output goes to `bin\` (`multilink.dll`, `multilink_inject.exe`, `multilink.ini`). The build also
runs the unit tests in `tests\selftest.cpp`. `bin\multilink.ini` is only created on the first
build, so later builds keep your edits.

## Use

1. Edit `bin\multilink.ini`. Set `adapters=` to your adapter names from `Get-NetAdapter`. Each
   adapter needs its own IPv4 default gateway. Repeat an adapter to give it a bigger share, matched
   to its speed (for example `Ethernet,Ethernet,Ethernet,Ethernet,Wi-Fi,Ethernet 5` for 90/20/20 Mbit/s).
2. Run the injector. It asks for administrator rights.
   - **HoYoPlay:** close HoYoPlay, then run `multilink_inject.exe hoyoplay`. It starts HoYoPlay
     itself, which it must do so the HTTP/2 switch catches sophon's start-up. Then click Download
     or Resume in HoYoPlay.
   - **Steam:** start Steam normally, then run `multilink_inject.exe steam`.
3. Check `bin\multilink.log`. You should see `... patched` and a per-adapter connection count every
   minute. Task Manager → Performance shows the traffic on each adapter.

Run the injector again whenever the launcher restarts or an adapter's IP changes. Adapter IPs are
read once, at start-up.

## Tools

| Folder | What |
|---|---|
| `tools/speedtest` | Speed test built on [Plexo](https://github.com/anmolkapil/plexo)'s own network layer (unmodified source, pinned commit). It measures each network alone, then all of them combined, against the HoYo CDN. Run `setup.ps1` once, then `node dist\speedtest.mjs [--streams 8] [--seconds 10] [--networks "Ethernet,Wi-Fi"]`. |
| `tools/hoyo-links` | `get-links.ps1 -Game hkrpg\|hk4e\|nap\|bh3` writes the official full-package links from the HoYoPlay API, for use in any download manager. `verify.ps1 -Folder <dir>` checks their size and MD5. The full packages can be older than the live version, and the launcher patches the rest. |

## Limits

- IPv4 TCP only. QUIC/UDP and IPv6 connections use the default route.
- The split is per connection, not per packet. A single connection never gets faster than one link.
- Connections opened before the patch is applied use the default route.
- The DLL stays loaded until the launcher exits, because its hooks must stay reachable.
- `Sophon_InitEnv`'s prologue and the import layout were checked on HoYoPlay 1.18.0.380 and
  Steam 11.23.65.88. Other builds may need the checks updated. The log says when a check fails.
