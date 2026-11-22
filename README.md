# launcher-multilink

Download game files over several internet connections at once (Ethernet + Wi-Fi + USB phone
tethering, and so on) from inside the game launcher itself. No proxy, no VPN, no server.

It is a small Windows tool for people who have more than one working internet link and whose game
launcher only uses one of them. A DLL is loaded into the launcher process and makes the launcher's
new download connections leave through the different adapters in turn.

**Only these three launchers are covered.** Any other launcher needs a new profile in the ini file and
may not work at all.

| Launcher | Games | Measured result |
|---|---|---|
| HoYoPlay 1.18.0.380 | Honkai: Star Rail, Genshin Impact, Zenless Zone Zero, Honkai Impact 3rd | Star Rail 4.7.0 download: about 98.6 Mbit/s on one 100 Mbit/s LAN link, about 138 Mbit/s with LAN + guest Wi-Fi + USB phone tethering |
| Kuro launcher 3.1.0.1 (`launcher_main.exe`) | Wuthering Waves, including the Steam release (it updates through this launcher, not through Steam) | About 98 Mbit/s on one link, 117-119 Mbit/s with LAN + Wi-Fi |
| Steam (steamclient64.dll 11.23.65.88) | Games that Steam itself downloads | Game update: 94.4 Mbit/s on one link, 119-120 Mbit/s with LAN + Wi-Fi |

All numbers come from the author's own machine (Windows 11 x64, November 2026). All HoYoPlay games
use the same download engine (`sophon.dll`), but only Star Rail was measured.

**It only helps when each link has its own bottleneck**: different ISPs, a phone on mobile data, or
a network that caps every connection or session separately. Two links behind the same router uplink
gave about 0% gain in testing.

## Quick start

1. Get the code: `git clone` this repository, or use "Code, Download ZIP" on GitHub and unpack it.
   Only `multilink.example.ini` is in the repository; your own `multilink.ini` is created in step 2.
2. Open a normal (non-administrator) Command Prompt in the project folder and run `build.bat`.
   The last line must say `build ok`. The first build copies `multilink.example.ini` to
   `multilink.ini`.
3. Edit `multilink.ini`: set `adapters=` to your adapter names (see "Set up") and set `enabled=true`
   in the section of your launcher. Fix its `start=` path if the launcher is installed elsewhere.
4. Pause any running download first. Then run `multilink_inject.exe` as administrator (right-click,
   Run as administrator). If HoYoPlay is open, the injector closes and restarts it. Follow what it
   prints, then resume the download in the launcher.
5. Open `multilink.log` and check that the adapters were patched (see "Check that it works").

## Requirements

- Tested on Windows 11 x64 only. Windows 10 is not claimed to work.
- Two or more network adapters that are connected and each have their own IPv4 default gateway.
- Administrator rights (see "Why administrator").
- To build: Visual Studio 2022 with the C++ desktop workload. No other dependencies.
- For `hook=inline` (Wuthering Waves): `ws2_32.dll` version 10.0.26100.1, the only version it was
  measured on. On other versions the hook is skipped and the log says so. Check yours in PowerShell:
  `(Get-Item C:\Windows\System32\ws2_32.dll).VersionInfo.ProductVersion`

## Glossary

| Term | Meaning |
|---|---|
| Adapter | One network interface of your PC: a LAN port, a Wi-Fi card, or a USB-tethered phone. |
| Default gateway | The router address an adapter sends internet traffic to. An adapter without an IPv4 default gateway cannot carry internet traffic and is skipped. |
| Profile | One `[section]` of `multilink.ini` that describes one launcher. |
| Import table (IAT) patch | Changing the address table through which a module calls `bind` and `connect`, so only that module is affected (`hook=iat`). |
| Inline hook | Overwriting the first bytes of `ws2_32!bind` with a jump, so the whole process is affected (`hook=inline`). |
| Late injection | Injecting into a launcher that is already running, instead of starting it through the injector. |

## Build

```bat
build.bat
```

`build.bat` calls the MSVC x64 environment script at this hard-coded path (Community edition):

```
C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
```

If it is missing, the build prints `ERROR: Visual Studio 2022 vcvars64.bat not found. Edit the path on
line 3 of build.bat.` and exits with 1. For Professional, Enterprise, Build Tools or another install
location, edit that path on line 3.

The build writes `multilink.dll` and `multilink_inject.exe` into the project folder and runs the unit
tests from `tests\selftest.cpp` (the build fails if they fail). On the first build it creates
`multilink.ini` from `multilink.example.ini`; later builds leave your edits alone. Intermediate files go
to `obj\`. The DLL, exe, ini and log are not committed.

Run `multilink_inject.exe` from this folder (or a copy with the DLL and ini next to it): it looks for
`multilink.dll` and `multilink.ini` next to itself, and `multilink.log` is written there too.

## Set up

0. **Optional: check that your links can add speed.** The speed test measures each network alone and
   then all together. It needs git and Node.js 20 or newer (see "Tools"). Skip it if you just want to try.
1. **Connect every link first** (plug in the phone and turn on USB tethering, join the Wi-Fi), then
   **find your adapter names.** In PowerShell run `Get-NetAdapter` and note the `Name` column, for
   example `Ethernet` and `Wi-Fi`. Each adapter must be up and have its own IPv4 default gateway,
   otherwise the log says `skipped: down or no IPv4 gateway`. At least two distinct adapters must be
   usable, or nothing is patched.
2. **Edit `multilink.ini`.** Set `adapters=`, set `start=` to the launcher's exe, and set
   `enabled=true` in the sections you want. All three profiles ship with `enabled=false`.
3. **Run `multilink_inject.exe`** as administrator. With no arguments it handles every section with
   `enabled=true`. `multilink_inject.exe <section>` (for example `multilink_inject.exe steam`) handles
   one section even if it is disabled.
4. **Resume or start the download in the launcher**, as described below.

### Worked example: Ethernet + phone USB tethering, Wuthering Waves

`Get-NetAdapter` shows `Ethernet` (about 90 Mbit/s) and `Ethernet 5` (a phone tethered over USB,
about 20 Mbit/s). Adapter names are whatever Windows calls them on your PC; `Ethernet 5` is only an
example. Listing `Ethernet` four times gives it about four fifths of the connections.

```ini
[multilink]
adapters=Ethernet,Ethernet,Ethernet,Ethernet,Ethernet 5

[wuwa]
process=launcher_main.exe
enabled=true
hook=inline
start=<your install dir>\launcher.exe
autostart=true
late=reset
```

Run `multilink_inject.exe wuwa`. If you want a launcher that is not yet running to be started for you,
`autostart=true` allows it.

### Why administrator

HoYoPlay (`HYP.exe`) and the Kuro launcher (`launcher_main.exe`) run elevated, and a normal process
cannot inject into an elevated one. That is also why their profiles have `autostart=true`: if the
injector starts them, they come up elevated like the injector. Steam does not run elevated, so its
profile has `autostart=false`; start Steam normally and inject late (or see Troubleshooting). The
injector also needs the rights to reset a launcher's open TCP connections (`late=reset`). It exits
with a message if it is not elevated.

### What happens in each case

- **The launcher is not running.** With `autostart=true` the injector starts it from `start=` and
  injects. Otherwise it tells you to start the launcher normally and run the injector again.
- **The launcher is already running.** The injector samples its I/O for 3 seconds. At 1 MB/s or more
  it refuses: pause the download in the launcher and run the injector again. Then `late=` decides what
  happens (see the options table).
- **Resume afterwards**, in the launcher: HoYoPlay: Restore Download (after a restart) or Download;
  Kuro launcher: Continue download; Steam: Resume on the Downloads page. Buttons for any other launcher
  are not known.
- **HoYoPlay specifically** fixes its protocol at start-up, so it needs a fresh start through the
  injector. With `late=restart` the injector closes HoYoPlay and its child processes, starts it again
  and injects; paused progress is kept.

## Options in `multilink.ini`

Switches (`enabled`, `autostart`) are on for `1` or `true` (any case) and off for anything else,
including a missing key. Lines starting with `;` are comments.

### `[multilink]`

| Key | Allowed values | Default | Meaning |
|---|---|---|---|
| `adapters` | Comma-separated adapter names from `Get-NetAdapter` (case-insensitive), at most 511 characters | empty, so nothing is patched | New connections go to these adapters round-robin. List an adapter several times to give it a bigger share, matched to its speed. |

### Launcher sections (`[hoyoplay]`, `[steam]`, `[wuwa]`, or your own name)

The section name is only a label for `multilink_inject.exe <section>`. `[multilink]` is reserved.

| Key | Allowed values | Default | Meaning |
|---|---|---|---|
| `process` | Exe file name, e.g. `HYP.exe` | none (required) | The launcher process to inject into. The DLL also uses it to find its own section. |
| `enabled` | `true`/`false`, `1`/`0` | off | Whether a no-argument run handles this section. |
| `module` | A DLL file name, e.g. `sophon.dll` | none | The download engine whose imports are patched (`hook=iat`). The DLL waits up to 10 minutes for it to load. Ignored with `hook=inline`. |
| `hook` | `iat` or `inline` | `iat` | `iat` patches `bind` and `connect` in the import table of `module`. `inline` patches `ws2_32!bind` for the whole process, for .NET launchers that have no import table entry to patch. |
| `start` | Full path to the launcher exe | empty | Used when the launcher has to be started. This is the launcher's own starter exe (usually `launcher.exe`), which then spawns the `process=` exe; that is why the two names differ. The injector refuses a path that is not an existing file. |
| `autostart` | `true`/`false`, `1`/`0` | off | Allow the injector to start (and for `late=restart`, restart) the launcher. Use only for launchers that run elevated anyway. |
| `late` | `reset` or `restart` | `reset` | For an already running, idle launcher. `reset`: inject, wait up to 15 s for the hooks, then drop its open IPv4 connections to public addresses so they reconnect through the hook. `restart`: close the launcher and its children, start it again, inject; needs `autostart=true`. |
| `reset_ports` | Remote ports, comma-separated, ranges allowed (`80,443`, `8000-8080`) | empty, meaning all | Limits `late=reset` to these ports. |

Profiles in the example file (all `enabled=false`; change `start=` to your install path):

| Section | `process` | Hook | `autostart` | `late` | Notes |
|---|---|---|---|---|---|
| `[hoyoplay]` | `HYP.exe` | `iat` on `sophon.dll` | true | `restart` | Also turns HTTP/2 off. |
| `[steam]` | `steam.exe` | `iat` on `steamclient64.dll` | false | `reset` | `reset_ports=80,443`: dropping Steam's own connection (TCP 27015-27050) puts it in offline mode. |
| `[wuwa]` | `launcher_main.exe` | `inline` | true | `reset` | Also used by the Steam release of Wuthering Waves. |

## How it works

1. The injector loads `multilink.dll` into the launcher process.
2. The DLL reads `multilink.ini`, finds the section whose `process=` is the current exe, resolves the
   adapters to IPv4 addresses and patches `bind` and `connect`.
3. Every new outgoing IPv4 TCP connection to a public address gets a source address from the next
   adapter in the list. Windows sends a socket out of the adapter that owns its source address, so
   connections spread over all links. Loopback, private, link-local, CGNAT and multicast destinations,
   and every other module's traffic, are left alone.
4. **HoYoPlay only: HTTP/2 is turned off.** `sophon.dll` embeds Chromium's network stack; over HTTP/2
   it sends every chunk through one connection, which cannot be split. The DLL hooks `Sophon_InitEnv`
   when `sophon.dll` loads and rewrites `"enable_http2": true` to `false`. If sophon loaded before the
   DLL, the log says "http2 stays on. Start HoYoPlay through multilink_inject.exe".
5. **`hook=inline` (Wuthering Waves).** The Kuro launcher downloads through .NET `HttpClient`, which
   reaches Winsock without an import table. The DLL replaces the first 5 bytes of `ws2_32!bind` with a
   jump to a stub and keeps a trampoline. .NET binds every socket before connecting, so the hook picks
   the adapter there, including dual-mode IPv6 sockets, which get an IPv4-mapped source address. This
   applies to the whole launcher process.

### Supported builds

Hooks that rewrite code check the bytes first and are skipped, with a log line, when they differ:

| Hook | Checked against |
|---|---|
| `Sophon_InitEnv` prologue (HTTP/2 off) | `sophon.dll` of HoYoPlay 1.18.0.380 |
| `ws2_32!bind` prologue (`hook=inline`) | `ws2_32.dll` 10.0.26100.1 |

The import-table patch (`hook=iat`) has no version check; it needs the module to import `bind` and
`connect` from `ws2_32` and logs `IAT slots not found` otherwise. It was measured on Steam 11.23.65.88.

## Check that it works

1. Open `multilink.log` (next to the DLL). Look for `adapter '<name>' -> <ip>` for each adapter, then
   `... patched: N round-robin slots` (`inline-hooked` for `hook=inline`, `Sophon_InitEnv@... hooked`
   for HoYoPlay).
2. Start or resume the download. About once a minute the log gets a `connections per adapter:` line.
   Every listed adapter should count up.
3. Task Manager, Performance tab: each adapter should show download traffic.

The log is appended to and never rotated; delete it when it gets large.

## What it touches, and trust

- It only touches processes whose exe name matches `process=` in an enabled profile (or the profile
  you name on the command line). For each one it writes the path of `multilink.dll` into the process
  and starts `LoadLibraryW` there; that is the whole injection.
- It may close and restart a paused launcher (`late=restart`) and drop that launcher's open
  connections (`late=reset`). Downloads should be paused first; the injector refuses otherwise.
- Nothing is uploaded anywhere. The only files it writes are `multilink.log` and the build output
  (plus `tools\speedtest\plexo`, `node_modules` and `dist` if you run the speed test).
- There are no prebuilt binaries. Build from source, and read `src/injector/injector.cpp` first: it is
  the code that runs as administrator.
- Antivirus software may flag DLL injection, because that is what this does. A launcher with its own
  anti-tamper protection may also object to a foreign DLL.
- The speed test clones Plexo from GitHub and downloads test data from the HoYo CDN.
  `get-links.ps1` calls the public HoYoPlay API.
- Use at your own risk.

## Stop and undo

Nothing is installed. Closing the launcher unloads the DLL, and the launcher is back to normal on its
next start. While the launcher runs, the DLL stays loaded (its hooks must stay reachable).
`enabled=false` only affects future injector runs; it does not remove a DLL that is already loaded.
A launcher the injector started (or restarted) runs as administrator until you close it and start it
normally again. To remove everything, close the launchers first (a loaded DLL cannot be deleted), then
delete the project folder.

Run the injector again whenever the launcher restarts or an adapter's IP changes: adapter IPs are read
once, at start-up.

## Tools

| Folder | What it does |
|---|---|
| `tools/speedtest` | Speed test built on [Plexo](https://github.com/anmolkapil/plexo)'s network layer (unmodified source, pinned commit). Measures each network alone, then all combined, against the HoYo CDN. |
| `tools/hoyo-links` | Writes the official full-package download links of a HoYoverse game from the HoYoPlay API, and checks downloaded files. |

**Speed test** (needs git and Node.js 20+). Once, from `tools\speedtest`:

```powershell
powershell -ExecutionPolicy Bypass -File setup.ps1
```

It clones Plexo at the pinned commit, installs dependencies and builds `dist\speedtest.mjs`. Then:

```powershell
node dist\speedtest.mjs [--streams 8] [--seconds 10] [--networks "Ethernet,Wi-Fi"] [--url <file>] [--all]
```

`--streams` is 1-32 (default 8), `--seconds` 3-120 (default 10), `--networks` limits the test to named
networks, `--url` uses a file of your choice instead of the HoYo CDN file, `--all` also includes
networks classified as "other" or "bridge". If the combined result is clearly above the best single
network, multilink can help.

**HoYo links**, from `tools\hoyo-links`:

```powershell
powershell -ExecutionPolicy Bypass -File get-links.ps1 -Game hkrpg
powershell -ExecutionPolicy Bypass -File verify.ps1 -Folder <download folder>
```

`-Game` is `hkrpg` (Star Rail, default), `hk4e` (Genshin Impact), `nap` (Zenless Zone Zero) or `bh3`
(Honkai Impact 3rd). `-Audio en-us,ja-jp` picks audio packs (default `en-us`); `-Index` picks one entry
when the API lists several. `get-links.ps1` writes `links.txt` and `md5.txt` there; `verify.ps1` checks
size and MD5 of each file and exits 1 if any need downloading again. Full packages can be older than
the live version; the launcher patches the rest.

## Troubleshooting

| Symptom | What to do |
|---|---|
| "NOT running as administrator" | Right-click `multilink_inject.exe`, Run as administrator. |
| "missing ...multilink.dll" | Run `build.bat` and use the exe from the project folder. |
| `ERROR: Visual Studio 2022 vcvars64.bat not found` | Edit the path on line 3 of `build.bat`. |
| `start= ... is not a file` | Set `start=` in that section to the launcher exe's full path. |
| "... is not running. Start it normally" | The section has no `autostart=true`. Start the launcher, then run the injector. |
| "is downloading ... Pause the download" | Pause in the launcher, wait a few seconds, run again. |
| "late=restart needs autostart=1" | Set `autostart=true` in that section. |
| Log: `fewer than 2 usable adapters, nothing patched` | Check names against `Get-NetAdapter`; each adapter must be up with its own IPv4 default gateway. |
| Log: `prologue differs ... not installed` / `left as is` | Your HoYoPlay or ws2_32 build differs from the checked one, so the hook is skipped. |
| Speed does not change | The links probably share one bottleneck; run the speed test. For HoYoPlay, make sure it was started through the injector. |
| Steam shows "offline" after a late injection | Steam sometimes drops to offline mode even with `reset_ports=80,443`. Close Steam completely (tray icon, Exit), set `autostart=true` and a correct `start=` under `[steam]`, run `multilink_inject.exe steam`, resume the download. This is a deliberate exception to "autostart only for elevated launchers": Steam started this way runs as administrator until you exit it and start it normally. |

## Limits

- IPv4 TCP only. QUIC/UDP and IPv6 use the default route (except IPv6 sockets that `hook=inline`
  binds to an IPv4-mapped address).
- The split is per connection, not per packet: one connection never goes faster than one link.
- Connections opened before the patch is applied use the default route.
- Only the three launchers above are covered; code-rewriting hooks work only on the builds in
  "Supported builds".

## License

No license has been chosen yet.
