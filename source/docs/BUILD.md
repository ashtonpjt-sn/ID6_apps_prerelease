# Build and deploy

Run commands from `source/`. Host tests use a C compiler, Make, Go as declared in `go.mod`, and Python 3. No car or BMW files are needed for these tests.

```sh
go test -race ./...
make -C native test
python3 tools/test_probe_signals.py
```

`make -C native test` also drives every plugin class against a host stand-in for the HMI framework and every worker against a simulated gateway. See [NEW_APPS.md](NEW_APPS.md#tests).

## Native payload

1. Prepare a licensed QNX SDP 6.5 Linux x86 toolchain with ARM GCC 4.4.2. Set `QNX_HOST`, `QNX_TARGET`, and `QNX_CONFIGURATION` if your installation differs from the paths in `native/build.sh`.
2. On a parked, authorized reference head unit, enable temporary SSH, confirm the host fingerprint, and read these two framework files into your private `native/frameworks/` directory. These are link and ABI inputs. The resulting installer uses the copies already on each car. Example for macOS or a Unix shell:

```sh
mkdir -p native/frameworks
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libAppFramework.A.so' > native/frameworks/libAppFramework.A.so
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libCoreFramework.A.so' > native/frameworks/libCoreFramework.A.so
shasum -a 256 native/frameworks/*.so
```

   Replace `HEAD_IP` with the head-unit SSH IP. Use a binary-safe transfer. If the old SSH server needs a legacy algorithm, configure it for that specific ENET connection. Use the head unit's existing library copies as link inputs. Where SDK linker versions differ, set `NBTEVO_RUNTIME_DIR` to a private directory with matching `libm.so.2`, `libsocket.so.3`, and `libstdc++.so.6` from the authorized unit or its reference image. The build also accepts `NBTEVO_FRAMEWORKS_DIR`.
3. Generate the RHMI resources with Python and Pillow. The scripts use host fonts, so image bytes can differ across hosts. DejaVu fonts are the fallback on Linux.

```sh
python3 -m pip install Pillow
make -C native assets
sh native/build.sh
python3 tools/stage_payload.py
python3 tools/stage_payload.py --check
```

The script compiles the native libraries, worker executables, and the small ARM file-operations helper. Staging validates the ARM ELF format, manifest IDs, and image references. It packages only the app folders and helper.

## New firmware

Use the [unlisted firmware runbook](../../AgentReadMe.md#bring-up-an-unlisted-firmware) and [read-only capture tool](../tools/capture_headunit.py) to collect the platform, display, and framework evidence before building a candidate profile.

Read `/opt/sys/etc/nbt_version.txt`, `uname -a`, and `/opt/sys/bin/LayerManagerControl get screen 0` over SSH. Record the on-screen version and known iDrive pixel resolution if the corresponding automatic query fails. Compare the two framework hashes with `profiles` in `install.go`. For a different pair, compare static ABI evidence with the already tested reference using `native/tools/audit_framework_abi.py` with `pyelftools` and `capstone`. Verify all imported symbols, constructor signatures, and vtable slots, then run a parked load and lifecycle test on that firmware. Static similarity alone does not establish runtime compatibility. Add a firmware entry only with matching evidence. Keep firmware approval separate from the vehicle's DME/EGS signal mapping.

For example, compare the candidate against an independently preserved reference library pair:

```sh
python3 native/tools/audit_framework_abi.py \
  --reference-dir /private/reference-frameworks \
  --installed-dir /private/candidate-frameworks \
  --plugin native/out/libBoostDeck.so --report /private/boostdeck-abi-report.json
```

Repeat for ShiftDeck and MHD Maps and inspect the report's limitations. The tool compares selected instruction prefixes and vtable relocation slots. It does not simulate HMI calls or prove that a matched framework works at runtime.

## Desktop and car

With the payload staged, `go build .` builds the local app. On macOS with Xcode command-line tools, `./build-release.sh` creates the universal macOS ZIP and Windows x64 ZIP with SHA-256 checksums. On Linux, native artifacts can be built and staged first, then copied privately to the macOS packaging checkout.

Use the [installer guide](../README.md#connect-and-install). The installer checks compatibility, stages complete directories outside the plugin scan path, verifies bytes, and publishes with rollback. Keep the reported backup path. Restart iDrive with the power/volume knob only once all apps are verified, and check the menus while parked. For a failed install, leave iDrive running and inspect the status and backup markers. The recovery button is offered only for an empty lock with staging evidence and no live apps or publication markers.

The live plugin base is `/var/opt/hmi/data/ID5/Plugins`. Its app directories are `BoostDeck`, `ShiftDeck`, `MhdMaps`, `WarmUp`, `PerfTimer`, and `FaultScan`. The installer writes `gateway-ip.txt` beside each worker. Keep other manifests and stock files intact. On rollback, move only the affected plugin out of the scan path and restore the corresponding backup before restarting.
