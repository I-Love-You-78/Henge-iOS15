<p align="center">
  <img src="docs/icon.png" width="120" height="120" alt="Henge">
</p>

<h1 align="center">Henge</h1>

<p align="center">macOS on your iPhone and iPad.</p>

<p align="center">
  <a href="#install">Install</a>
  ·
  <a href="docs/architecture.md">How it works</a>
  ·
  <a href="https://itslevii.dev">Website</a>
  ·
  <a href="https://github.com/Leviidev/Henge/releases">Releases</a>
</p>

<p align="center">
  <a href="https://github.com/Leviidev/Henge/releases"><img src="https://img.shields.io/github/downloads/Leviidev/Henge/total?style=flat-square&color=2E6BFF&labelColor=111111" alt="Downloads"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/Leviidev/Henge?style=flat-square&color=2E6BFF&labelColor=111111" alt="GPL-2.0-or-later"></a>
</p>

Download macOS, install it on the device, and use it.

- **Own emulator** — iOS gives apps no hypervisor, so Henge runs the Mac's arm64 code through its own JIT, with its own MMU and devices. It doesn't use QEMU or Virtualization.framework.
- **Installs on the device** — Henge downloads Apple's restore image and restores it the way a Mac does. No Mac needed, and nothing is decrypted or patched.
- **Graphics** — macOS draws through a virtual GPU, and Henge replays its commands with Metal. Safari and Electron apps like Discord draw on it.
- **Touch** — The screen is a trackpad: hold to drag, two fingers to scroll. Esc, Tab, Control, Option and Command sit above the keyboard.
- **Keyboard and mouse** — On iPad, a Magic Keyboard or a Bluetooth keyboard and mouse drive the Mac directly.
- **External display** — During a Mac session, connect a display with a video-capable USB-C cable or a USB-C/HDMI dock. The mode activates automatically: the monitor shows the Mac, while the iPhone or iPad keeps its trackpad and keyboard controls. Disconnect to return the image to the device. The existing guest resolution is preserved. On iOS 17–26 the same happens through an older iOS route, not yet tried on hardware; outside a Mac session the display mirrors the device. USB-C video is user-confirmed on an iPhone 17 Pro running iOS 27 (development build 0.1/2).
- **Sharing** — Copy and paste between macOS and the device, and a shared folder that shows up in Files and on the Mac.
- **Fits the screen** — On iPad, a new Mac gets a screen the shape of the iPad's, so there are no black bars.

On an iPhone 17 Pro, macOS reaches the lock screen in about 15 seconds. Discord installs, opens in about 15 seconds, logs in and sends messages.

## This fork: iOS 15 and A9 (iPhone 6s / SE)

This fork adds an **iOS 15+** build that runs on **A9 and newer** arm64 devices —
the iPhone 6s and first SE included — with a GitHub Actions workflow that builds
the `.ipa` without a Mac:

- The engine no longer copies Armv8.1+ instructions into blocks on a CPU that
  cannot run them. `core/hostcaps.c` probes the host per feature (sysctl, else by
  running the instruction under a SIGILL guard) and anything it cannot run is
  emulated in C: atomics in `core/memop.c` (`LDADD`, `SWP`, `CAS`, `LDAPR`,
  `LDAPUR`…), and `core/simd.c` for CRC32, SDOT/UDOT, FMLAL/FMLSL, SQRDMLAH/SH,
  EOR3/RAX1/BCAX/XAR and FJCVTZS. `tests/test_simd.c` runs every one of those
  natively and through the emulator on an Apple Silicon CI runner and compares all
  lanes: 866 cases pass.
- The app builds for iOS 15 (`ios/project-ios15.yml`), with a thin compatibility
  layer for what arrived in iOS 16 (`ios/Henge/Legacy/Compat.swift`) and a JIT
  screen for the TrollStore / jailbreak route that iOS 15–16 uses
  (`ios/Henge/Legacy/JITLegacy.swift`).
- Guest RAM is chosen in MB and capped for 2 GB devices; the code cache drops to
  256 MB there.
- `.github/workflows/ios15-build.yml` builds `Henge-iOS15.ipa`
  (`ios/scripts/package_ios15_ipa.sh`). The ipa is small on purpose: it holds the
  emulator, and macOS is downloaded from Apple on the device at install time.

Read [docs/ios15-a9-support.md](docs/ios15-a9-support.md) for what works, what does
not (Armv8.2 SIMD and 2 GB of RAM are the walls) and how to install it.

## Install

Sideload `Henge.ipa` from [Releases](https://github.com/Leviidev/Henge/releases)
with SideStore or AltStore, or install `Henge.tipa` with TrollStore or TrollStore
Lite. iOS or iPadOS 17 or later, on an A14 or M1 chip or newer. So far it's tested
only on an iPhone 17 Pro: iPad support is new and hasn't run on a real iPad yet.

- **JIT** is required: StikDebug, TrollStore (which stops at iOS 17.0), TrollStore Lite (which needs a jailbreak), a jailbreak on its own, or the StikJIT helper built into Henge (iOS 26 or later).
- **Storage:** about 43 GB free for macOS Sequoia. That's 17 GB for the download, which you can delete afterwards, and 26 GB for the Mac.
- **Memory:** a Mac gets 4, 6 or 8 GB. Sign Henge with the increased memory limit entitlement if your certificate has it.

Then open the Install tab, pick macOS Sequoia 15.6.1, and keep Henge open with the device plugged in. Tahoe 26.6.2 and macOS 27.2 are offered too, but they aren't tested yet.

## Sharing with the Mac

Do this once for each Mac: open Terminal on the Mac, then in Henge open Details and tap **Type the Set Up Command**. Press Return on the Mac, and choose Allow when macOS asks about `henge-agent` and local networks. It needs the Mac's keyboard layout to be U.S.

Then a Henge folder in the Mac's home folder is the same folder as Files › Henge › Shared. What you copy on the Mac is copied on the device too, and the paste button pastes from the device into the Mac.

## Known limits

- Signing in to an Apple Account inside macOS fails with a verification error.
- Graphics are tested on macOS's own apps, Chrome, Firefox, VS Code and Discord. Other apps may draw wrongly.
- For the first few minutes after you log in, macOS is busy starting up in the background, so apps open more slowly.
- Only one Mac can start per launch. Quit Henge and open it again to start another, and to start one again after restarting or shutting down macOS.
- iPadOS keeps some keyboard shortcuts for itself, such as Command-Tab.
- It's emulation, so it runs slower than a real Mac.

## Screenshots

<table>
  <tr>
    <td align="center" colspan="3"><img src="Screenshots/ipad.jpg" alt="Discord on an iPad"><br><sub>iPad</sub></td>
  </tr>
  <tr>
    <td align="center" width="33%"><img src="Screenshots/macs.png" alt="Macs"><br><sub>Macs</sub></td>
    <td align="center" width="33%"><img src="Screenshots/trackpad.png" alt="Trackpad"><br><sub>Trackpad</sub></td>
    <td align="center" width="33%"><img src="Screenshots/install.png" alt="Install macOS"><br><sub>Install macOS</sub></td>
  </tr>
</table>

<sub>Captured on a Mac: the app's own screens with sample data, at iPad and iPhone sizes, showing a frame from Henge's renderer.</sub>

## Building

To use Henge, install the [latest release](https://github.com/Leviidev/Henge/releases). Every change lands on `main` as it's made, so `main` can be unfinished between releases.

On a Mac with Xcode 27 or later and `xcodegen`. Henge takes StikJIT, on-device pairing, libslirp and glib from a built [Husk](https://github.com/Leviidev/Husk) checkout at `~/Documents/Coding/Husk` (set `HUSK` to use another path):

```bash
ios/scripts/fetch_deps.sh
tools/build_ios_restore.sh
make ios-lib
ios/scripts/package_ipa.sh
```

That produces `~/Desktop/Henge.ipa`, and `package_ipa.sh --trollstore` writes `Henge.tipa`.

`make` builds the engine for the Mac, `build/hengevm`, and `make test` runs its tests.

## Licence

GPL-2.0-or-later. Henge compiles in usbmuxd (GPLv2 or GPLv3) and code from Husk (GPL-2.0-or-later), so the shipped binary is a combined GPL work. It cannot go on the App Store. See [docs/licensing.md](docs/licensing.md).

macOS and the Mac's boot ROM are Apple's and aren't part of Henge. Henge downloads both from Apple when you install.
