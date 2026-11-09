# Warm-Up, Performance Timer, Fault Scanner, More vitals and gear

This fork adds three apps under **My Vehicle**, a second BoostDeck page, and a gear indicator in ShiftDeck. They use the same installer, firmware checks and ENET gateway as BoostDeck, ShiftDeck and MHD Maps. None of them has run on a car yet. Check each one while parked before you trust it.

| App | What it shows | How it gets the data |
| --- | --- | --- |
| **Warm-Up** | Oil and coolant temperature, a suggested rev limit, a progress bar and an estimate of the time to working temperature | OBD PID `5C` (oil) or DID `F45C`, BMW DID `4300` (coolant), OBD `0C` (RPM) |
| **Performance Timer** | 0–60 mph, ¼ mile with trap speed, 60–100 mph, 60–130 mph and 60–0 mph braking, each with last and best | OBD PID `0D` (speed), read as fast as the gateway answers |
| **Fault Scanner** | Stored fault codes in every control unit that answers, grouped by unit, eight lines per page | UDS `19 02 0C` to all units at once (functional address `DF`) |
| **BoostDeck → OPTIONS → More vitals** | Oil temperature, low-pressure fuel, battery voltage, fuel trims, ethanol content, engine load, misfires per cylinder | OBD service 01 and 06, plus the DID candidates listed in [SIGNALS.md](SIGNALS.md#more-vitals-candidates) |
| **ShiftDeck gear** | Current gear (1–8) between speed and RPM | Learned from the RPM-to-speed ratio, then saved |

## Warm-Up

The suggested limit starts at 3,000 rpm with oil at 30 °C (86 °F) or colder and rises to 6,000 rpm at 90 °C (194 °F). Above 90 °C the limit goes away. These numbers are a conservative rule of thumb, not a BMW specification. If the DME doesn't answer for oil temperature, the app estimates it as coolant minus 10 °C and says so on screen. The time estimate needs at least a minute of rising temperature.

## Performance Timer

Runs start and stop on their own:

- **0–60 mph and ¼ mile** start when the car moves off from a full stop. Lifting off or braking ends the run.
- **60–100 and 60–130 mph** start whenever the speed passes 60 mph going up.
- **60–0 mph braking** starts when the speed passes 60 mph going down and ends at a stop. Only stops averaging 0.35 g or more count, so ordinary stops don't overwrite a test.

The engine reports speed in whole km/h, so times are good to about ±0.1 s and braking distance to a few feet. The app interpolates between samples to get that close. A gap of more than 0.6 s in the data cancels the run in progress. Best results are saved in `perf-state.txt` in the app folder. **OPTIONS → Reset best results** clears them. Use it on a closed course only.

## Fault Scanner

**OPTIONS → Scan all modules** asks every control unit for codes that are pending or confirmed. Use **Next page** and **Previous page** to move through the list. The last scan is saved, so reopening the app shows it again without asking the car. Unit names cover the common F-series addresses. Any other unit shows as `Control unit XX`. The scanner shows codes without their text descriptions, so look them up in ISTA or your usual tool.

**OPTIONS → Clear fault codes** opens a page with two buttons:

- **Clear engine (DME) codes** clears control unit `12` only.
- **Clear codes in all control units** clears every unit that had faults in the last scan.

Clearing is the only feature in these apps that writes to the car. Several safeguards apply:

1. Each button has to be pressed twice within five seconds. Pressing a different button starts over.
2. Before clearing anything, the worker asks the DME for road speed. It clears only if the DME answers with exactly 0 km/h. With no answer, it clears nothing.
3. "All units" first sends one functional `14 FF FF FF`, as a workshop tester does. Any unit that doesn't confirm is then asked directly. If it refuses (for example NRC `22` with the engine running), the worker retries once in the extended session (`10 03`) and then returns to the default session (`10 01`).
4. A fresh scan follows straight away. Codes that come back at once are still present, so the fault is real.

Clearing also erases freeze-frame data that a workshop might want. Read and note the codes first.

## More vitals

These values come from public OBD definitions and the candidate list in idrive7-apps (MIT). Each one is shown only when the reply is the right length and the value is in a plausible range, otherwise `--`. None of them has been checked on a Gen 1 B58 yet. The repo's reference signals come from an F30 340i. On a G-series car such as a G01 X3 M40i, the DME (`12`) and EGS (`18`) use the same diagnostic addresses, but the DME software differs, so use the read-only probe to check each value. Compare them with MHD or another logger before relying on them, and add a DID to [`vitals.c`](../native/src/vitals.c) if your DME refuses one.

## ShiftDeck gear

ShiftDeck has no gear signal, so it learns one. On steady stretches above 40 km/h (25 mph), each RPM-to-speed ratio is compared with the ZF 8HP gear ratios (5.000, 3.200, 2.143, 1.720, 1.314, 1.000, 0.822, 0.640). The single factor that lines up with at least three different gears becomes the car's calibration. That factor is the final drive combined with tire size. It's saved in `gear-calibration.txt`, so the gear shows straight away on later drives. Until then, and while shifting or slipping, the screen shows `-`. A manual gearbox has different ratios, so it will keep showing `-`.

## Files the apps write on the head unit

| File | Written by |
| --- | --- |
| `Plugins/PerfTimer/perf-state.txt` | Best Performance Timer results |
| `Plugins/FaultScan/last-scan.txt` | The last fault scan |
| `Plugins/ShiftDeck/gear-calibration.txt` | The learned gear factor |

If the HMI can't write to its plugin folder, the apps still work but forget these between visits. Reinstalling replaces the app folders, which also resets these files.

## Code layout

| File | Purpose |
| --- | --- |
| [`native/src/hsfz.c`](../native/src/hsfz.c) | HSFZ transport moved out of `collector.c`, plus functional requests, shared gateway sessions and the worker output lines |
| [`native/src/drive_core.c`](../native/src/drive_core.c) | Warm-Up advice, run timing and gear learning, with no I/O |
| [`native/src/text_app.h`](../native/src/text_app.h) | Plugin base that runs a worker and copies its `T`/`I`/`V` lines into RHMI models |
| `native/src/warm_up*.c*`, `perf_timer*.c*`, `fault_scan*.c*`, `vitals.c` | Each app's worker and plugin |
| [`native/tools/make_text_apps.py`](../native/tools/make_text_apps.py) | RHMI XML, manifests and images for the three new apps |

The apps use connectors `105`, `106` and `107`. The installer refuses to install if another plugin already uses one of them.

## Tests

`make -C native test` now also runs:

- `test-drive-core`: warm-up rules, run timing on simulated launches, rolls and stops, and gear learning
- `test-plugin-*`: each plugin class against [`tests/mock_bmw.cpp`](../native/tests/mock_bmw.cpp), a host stand-in for the HMI framework, with scripted workers
- `tests/test_text_apps.py`: each worker against a simulated HSFZ gateway, including every clear-code safeguard

All of these run on a PC. They don't prove the plugins load on a head unit. A parked test on the car is still needed for that.
