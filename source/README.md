# NBT Evo ID6 Apps

[AgentReadMe](../AgentReadMe.md) covers setup, automation, and unlisted firmware.

Source for **BoostDeck**, **ShiftDeck**, **MHD Maps**, **Warm-Up**, **Performance Timer**, and **Fault Scanner**, plus the Mac and Windows x64 ENET installer. This is a parked preview for selected ARM/QNX NBT Evo builds. Firmware compatibility, screen geometry, and engine signal compatibility are checked separately.

- [Start here if a gauge is blank](docs/PORTING.md)

BoostDeck offers the original per-gauge palette under **OPTIONS → Gauge color → Multicolor**. ShiftDeck uses a full-width RPM bar. Both workers release the diagnostic gateway between short sessions and retry after connection loss.
- [Warm-Up, Performance Timer, Fault Scanner, More vitals and gear](docs/NEW_APPS.md)
- [Build, package, install, and recover](docs/BUILD.md)
- [Vehicle signals and current decoder assumptions](docs/SIGNALS.md)
- [Contributor and agent workflow](AGENTS.md)

| Where | What to edit |
| --- | --- |
| [native/tools/](native/tools/) and [native/plugin/](native/plugin/) | Layout generators, RHMI XML, button actions, and connector manifests |
| [native/src/collector.c](native/src/collector.c) | ECU address, diagnostic requests, polling, and signal validity |
| [native/src/gauge_core.c](native/src/gauge_core.c) | Length, scaling, bounds, stale data, and unit conversions |
| [tools/capture_headunit.py](tools/capture_headunit.py) | Read-only firmware and framework capture for listed and unlisted builds |
| [tools/probe_signals.py](tools/probe_signals.py) | Read-only ENET probe for named OBD PIDs and BMW DIDs |
| [install.go](install.go), [display.go](display.go), and [native/fileops.c](native/fileops.c) | Framework matching, display scaling, installation, and recovery |

The source and layouts are MIT licensed. ShiftDeck draws inspiration from Redline. MHD Maps uses the request documented by the upstream project and ships with a text heading. [License](LICENSE) · [Upstream attribution](third-party/idrive7-apps-LICENSE) · [Upstream map protocol](https://github.com/cauliflowermossgrape/idrive7-apps/tree/main/apps/live_adjustments)

The source installer prompts for the head unit’s SSH password locally. Each builder supplies their own QNX SDK and reference-car frameworks. Car captures, credentials, vendor logos, framework binaries, and PSdZData belong outside the public tree.

Parked preview for `NBTevo_N18092I`, `NBTevo_V20153I`, and `NBTevo_Y24073J`. The installer puts **BoostDeck**, **ShiftDeck**, **MHD Maps**, **Warm-Up**, **Performance Timer**, and **Fault Scanner** under My Vehicle. It checks both on-car HMI framework fingerprints before uploading. The preview has passed static framework checks and desktop installer tests. It still needs an on-car test on each of these builds.

BoostDeck shows boost, AFR, timing, intake air, transmission, rail pressure, oil pressure, and coolant. Its BMW-specific signal definitions come from the reference 2016 340i. Some cars will return fewer signals or use different definitions. Compare the values with a known-good logger before relying on them. ShiftDeck reads standard OBD RPM and speed and starts with a 6,500 RPM shift-light target.

## Connect and install

1. Park the car, switch on the ignition, and plug a wired ENET cable into the OBD port and your computer. Use a USB Ethernet adapter if needed.
2. Enable temporary SSH with an unlock tool that supports the exact head unit. [Bavarian SSH Client downloads](https://ssh.bavarian-aa.uk/)
3. Open **NBT Evo Gauge Installer**. The Mac ZIP contains a universal app for Apple silicon and Intel. The Windows ZIP contains an x64 executable. Both include all the app payloads. The installer reads the head unit's compatibility data directly over SSH. The app opens its interface in your browser on localhost. The Mac build is ad-hoc signed and the Windows build is unsigned.
4. Click **Find car**. The gateway IP and head-unit SSH IP are separate. If discovery only fills the gateway, take the SSH IP from the unlock tool. For manual troubleshooting, `ifconfig` on macOS or `ipconfig` on Windows shows the computer's ENET adapter. On macOS, `arp -an` and on Windows, `arp -a` show ENET neighbors the computer has seen.
5. Click **Check compatibility**. The first successful SSH handshake fills in the head unit's host key fingerprint. Confirm the SSH IP belongs to your connected car and compare the fingerprint with a trusted copy if you have one. Enter the head unit’s SSH password and click **Trust this fingerprint and check**. The installer expects the same host key for later SSH connections in this session. If iDrive does not expose its firmware string over SSH, enter the full NBTevo code from the navigation version information screen. The installer checks the live framework fingerprints even when you supply the code.
6. Leave the display on **Automatic**, or select the known iDrive output resolution and click **Check compatibility**. Confirm that the installer shows the expected firmware and resolution, then click **Install all apps**. Keep iDrive on and ENET connected until the installer finishes.
7. Restart iDrive only when the status says **Installed and verified all apps**. While parked, hold the volume/power knob until the screen goes dark (about 30 seconds), then release and wait for iDrive to come back. Open BoostDeck, ShiftDeck, or MHD Maps under **My Vehicle**. Check the layouts, controller navigation, and taps on native buttons if the car has touch. Gauge data and map requests use the vehicle's diagnostic connection.

## MHD Maps

On a car already running a compatible [MHD+ multi-map tune](https://mhd-tuning.atlassian.net/wiki/spaces/MFS2/pages/11763743/MHD%2BSuite), open **My Vehicle → MHD Maps**. The four tiles send a map request with one press. For the parked test, select a slot you have configured for the fuel in the car. The confirmation text reflects the DME's acknowledgement of the request. Verify the active slot separately in MHD. A timeout leaves the outcome unknown, so check the active slot before another press.

## Display and touch

The original compatibility reports identify the firmware and HMI libraries. The installer tries to read the active central display resolution over SSH. **Automatic** recognizes 800 × 480, 1280 × 480, and 1440 × 540. You can also choose one of those directly, select the experimental 1920 × 720 layout, or enter an experimental custom resolution between 800–1920 pixels wide and 480–720 pixels high, with an aspect ratio between 5:3 and 3:1. Use the resolution sent by iDrive, which can differ from the advertised resolution of an aftermarket panel. If the installer detects a resolution, a manual choice must match it. The firmware and framework checks still apply. The experimental layouts are scaled from the existing designs and need a parked visual check.

Touch coding, when available in the factory CarPlay configuration, appears in the compatibility result. The gauge screens use native iDrive controls. The controller and OPTIONS button provide access to settings, and touch-equipped cars can test tapping the settings buttons. The installer leaves touch coding alone.

## Recovery

The installer stages and reads back every file before publishing. It verifies and runs a bundled head-unit helper for moving directories and removing the empty lock. If it replaces an existing app directory, it retains the old one in the backup directory displayed by the installer. Publication moves only these app folders. Save the backup path before restarting iDrive.

If publication stops, leave iDrive running and save the full status and staging path. The installer reports the failed step and the head unit's error when available. If it finds an existing install lock, it stops before uploading and displays a read-only report of the lock, live app folders, and previous transaction markers. Close other installer windows and save the full report before clearing anything. Keep any `old-*` backups. A reboot does not remove an install lock stored on disk.

If the report finds an empty lock, all apps still in staging for at least one transaction, and no live apps or publication markers, the installer offers **Clear lock and install**. Close every other installer, confirm no other computer is installing, and click that button. The installer checks the same conditions again before removing the empty lock. It uploads and verifies its bundled files and leaves the previous staging folders available. If the button is absent, save the report so the live apps and backups can be checked before any changes.

If an app misbehaves after restart, enable temporary SSH. Move the installed app directory out of `/var/opt/hmi/data/ID5/Plugins` into a separate recovery directory. Restore its `old-<App>` backup (for example `old-BoostDeck`) from the reported location if there was a prior install. Then restart iDrive while parked.
