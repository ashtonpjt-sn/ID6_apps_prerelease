# Port BoostDeck to another car

A gauge can show `--` even when the menu works. A different ECU may reject a diagnostic identifier, return a different number of bytes, or use different scaling. Timeouts, the wrong gateway, an ECU that is asleep, and stale data can look the same on screen.

## 1. Record the setup

Record chassis, model year, engine family, DME and EGS identifiers if available, head-unit firmware, display resolution, and which readings work. Record whether ignition is on and whether the engine is off or idling. Keep the test parked. Use the diagnostic gateway IP and the computer's wired ENET IP from **Find car**. The SSH IP is a separate endpoint.

For an HMI port, use the [build guide](BUILD.md) to check both framework fingerprints and display geometry. A working menu and a responsive tachometer or speed readout are useful control observations, but they do not validate a particular DME identifier.

### Split-node units

NBT Evo is a two-processor unit: the Jacinto (`hu-jacinto`) talks to ENET, and the OMAP (`hu-omap`) runs the HMI. On some cars the SSH unlock lands on the Jacinto instead of the OMAP. Confirm with `uname -a`: `hu-jacinto` means split-node. On that node `/opt/hmi/ID5/lib` and `/opt/sys/etc` are empty or absent.

Read the HMI side read-only over Qnet from the Jacinto:

- Firmware code: `cat /etc/nbt_version.txt` (the Jacinto's own copy), or `on -f hu-omap cat /opt/sys/etc/nbt_version.txt`.
- Framework libraries and HMI version: under `/net/hu-omap/fs/sda0/...`. The OMAP's mount point varies: look under both `/net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib` and `/net/hu-omap/fs/sda0/opt/hmi/ID5/lib`.
- OMAP commands (e.g. the display query): `on -f hu-omap sh -c "export LD_LIBRARY_PATH=/opt/sys/lib:/usr/lib:/lib; /opt/sys/bin/LayerManagerControl get screen 0"`.

The OMAP has its own ENET address (separate from the Jacinto's SSH address) and may be reachable directly; the gateway's head-unit lookup returns the Jacinto address. Read the OMAP address from the Jacinto with `on -f hu-omap /sbin/ifconfig` (the `169.254.x.x` entry on its HMI NIC). The [capture utility](../tools/capture_headunit.py) detects the Jacinto, picks the Qnet paths, and records `ssh_node`, `hmi_node_ip`, and `framework_dir` in its report.

Installing matters here too: the plugin directory `/var/opt/hmi/data/ID5/Plugins` lives on the OMAP. On a split-node unit, stage the app folders either over SSH to the OMAP's own address (`hmi_node_ip`) or over the Qnet write path from the Jacinto (`/var` resolves under `/net/hu-omap/fs/sda1/...`). The Jacinto's own filesystem is not the HMI.

## 2. Capture a few explicit reads

Exit BoostDeck and ShiftDeck and close other diagnostic clients so the probe has the gateway to itself. Start with RPM, standard OBD coolant, and the existing BMW coolant and oil-pressure candidates. Replace the example IPs with the detected IPs. This probe works with Python 3 on macOS or Windows and does not require head-unit SSH.

```sh
python3 tools/probe_signals.py --local 169.254.10.2 --gateway 169.254.10.3 --ecu 12 010c 0105 224300 22586f
python3 tools/probe_signals.py --local 169.254.10.2 --gateway 169.254.10.3 --ecu 18 22da12
```

To survey a whole engine description at once, pass a catalog instead of explicit requests: `--catalog tools/b58_catalog.json` sends each listed read once, with the same read-only checks and spacing, and decodes positive replies for a first sanity check; `--only 224a49,22f470` narrows it. The B58 catalog is built from idrive7-apps by `tools/import_dme_catalog.py` and lists candidates, not verified values.

On Windows use `py -3` if Python is installed with the launcher. The probe allows only explicit `01` and `22` reads, targets DME or EGS, and bounds and spaces its requests. Keep address, request, complete response, test conditions, and the independent reading for each candidate.

| Observation | Next check |
| --- | --- |
| RPM and other reads time out | Verify the direct ENET adapter and gateway, ignition, and diagnostic contention. |
| `7f 22 31` | This request was rejected as out of range. Look up the relevant job for the specific ECU variant. |
| `7f 22 22` or other rejection | Record the code and current conditions. Check the job's prerequisites. |
| Positive reply, gauge stays blank | Check the returned data length, endian order, sentinels, bounds, and staleness logic. |
| Reading looks plausible | Compare with an independent readout across multiple points before enabling the decoder. |

Use an ECU-matched diagnostic definition or capture the request from a trusted logger you are authorized to use. Match the measurement meaning, ECU address, identifier, length, byte order, sign, offset, units, and invalid sentinels. Coolant can have several sensors. Oil pressure may be actual pressure, target pressure, or a switch state. When supported, `01 05` is a useful coolant reference and decodes as one unsigned byte minus 40 °C. The existing gauge uses the BMW candidate `22 43 00`. There is no universal oil-pressure read in this project.

## 3. Make the smallest decoder change

- Keep the existing profile usable. If a new ECU uses a different layout or scaling, add a distinct profile gated by verified ECU identification. Avoid choosing a formula solely because its result falls inside the display range.
- Update the request and ECU address in `native/src/collector.c`, and the reply checks and conversion in `native/src/gauge_core.c`. Keep invalid or stale readings blank.
- Add a captured, de-identified positive fixture and a negative or invalid-length fixture in `native/tests/`. Run `make -C native test` and `python3 tools/test_probe_signals.py`.
- Change the layout only when needed. Use `native/tools/make_bars.py` or `make_drive_apps.py`, regenerate assets, and keep model IDs, image IDs, worker formats, and connector IDs consistent.
- Build and install with [BUILD.md](BUILD.md). While parked, compare the display with a trusted tool on the same car. Check the blank or stale state with diagnostics unavailable. Record the tested ECU and firmware separately from supported but untested combinations.

## 4. Port the layout to another panel size

The raw dashboard ZIP is laid out for 1280 × 480. Read the resolution iDrive drives with `LayerManagerControl get screen 0` (through `on -f hu-omap` on a split-node unit); it can differ from the panel's advertised size. For a panel with the same 8:3 aspect, such as 1440 × 540, rescale each packaged app folder:

```sh
python3 native/tools/rescale_layout.py BoostDeck BoostDeck-1440x540 --to 1440x540
```

The tool scales component positions and sizes in the layout XML and resamples the PNGs in the image archive by the same factor, so each image keeps its relationship to its box. It keeps every model, image, action, and connector ID and copies the manifest, plugin library, worker, and `gateway-ip.txt` byte for byte, so no native rebuild is needed. It refuses an aspect change and a layout that would leave the panel. On firmware that has not run the dashboard before, load the unmodified 1280 × 480 folders first so a framework problem is not mistaken for a layout problem, then swap in the rescaled folders.

Share a minimal signal table and sanitized examples. Avoid uploading full vehicle captures, identifying data, or framework libraries.
