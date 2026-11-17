# Build and deploy

Run commands from `source/`. Host tests use a C compiler, Make, and Python 3. No car or BMW files are needed for these tests.

```sh
make -C native test
python3 tools/test_probe_signals.py
```

`make -C native test` also drives the Warm-Up, Performance Timer, Fault Scanner, BoostDeck and ShiftDeck plugin classes against a host stand-in for the HMI framework, and their workers against a simulated gateway. See [NEW_APPS.md](NEW_APPS.md#tests).

## Native payload

If you need the toolchain for a native build, the [QNX SDP 6.5 archive](https://archive.org/details/qnxsdp-65.7z) includes the SDK and a license key generator, according to the archive listing.

1. Prepare a licensed QNX SDP 6.5 Linux x86 toolchain with ARM GCC 4.4.2. Set `QNX_HOST`, `QNX_TARGET`, and `QNX_CONFIGURATION` if your installation differs from the paths in `native/build.sh`.

```sh
mkdir -p native/frameworks
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libAppFramework.A.so' > native/frameworks/libAppFramework.A.so
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libCoreFramework.A.so' > native/frameworks/libCoreFramework.A.so
shasum -a 256 native/frameworks/*.so
```

   Replace `HEAD_IP` with the head-unit SSH IP. Use a binary-safe transfer. If the old SSH server needs a legacy algorithm, configure it for that specific ENET connection. On a split-node unit (`uname -a` reports `hu-jacinto`), `/opt/hmi/ID5/lib` is empty on the SSH node; read the libraries over Qnet under `/net/hu-omap/fs/sda0/...` instead (see [Split-node units](PORTING.md#split-node-units)). Use the head unit's existing library copies as link inputs. Where SDK linker versions differ, set `NBTEVO_RUNTIME_DIR` to a private directory with matching `libm.so.2`, `libsocket.so.3`, and `libstdc++.so.6` from the authorized unit or its reference image. The build also accepts `NBTEVO_FRAMEWORKS_DIR`.
3. Generate the RHMI resources with Python and Pillow. The scripts use host fonts, so image bytes can differ across hosts. DejaVu fonts are the fallback on Linux.

```sh
python3 -m pip install Pillow
sh native/build.sh
```

The script builds the native libraries and diagnostic workers. The dashboard diagnostic workers use the [MOST fallback](MOST.md), which loads the DSI library at runtime. The MHD+ Suite package also includes the HSFZ Exhaust Flap worker from PR #29 as `MhdMaps/exhaust-flap-worker-qnx`. Use `native/tools/merge_exhaust_flap_page.py` to assemble the submenu from the original flap artwork and the MHD+ Suite image archive. The packaged XML and matching image archives must be deployed together.

The script also builds Warm-Up, Performance Timer and Fault Scanner, BoostDeck's More vitals mode and ShiftDeck's gear estimate ([NEW_APPS.md](NEW_APPS.md)). Their layouts are in `native/plugin/`. `make -C native text-assets` writes the three new apps' image archives. To stage the five changed apps with the published dashboard images, the GEAR legend and `gateway-ip.txt`:

```sh
python3 native/tools/stage_apps.py --package ../packages/dashboard.zip --build native/out \
    --gateway GATEWAY_IP STAGE_DIR
```

For a panel other than 1280 × 480, rescale each staged folder with `native/tools/rescale_layout.py` ([PORTING.md](PORTING.md#4-port-the-layout-to-another-panel-size)).

## New firmware

Use the [unlisted firmware runbook](../../AgentReadMe.md#bring-up-an-unlisted-firmware) and [read-only capture tool](../tools/capture_headunit.py) to collect the platform, display, and framework evidence before building a candidate profile.

Read `/opt/sys/etc/nbt_version.txt`, `uname -a`, and `/opt/sys/bin/LayerManagerControl get screen 0` over SSH. Record the on-screen version and known iDrive pixel resolution if the corresponding automatic query fails. Compare the two framework hashes with the reference table in `AgentReadMe.md`. For a different pair, compare static ABI evidence with the already tested reference using `native/tools/audit_framework_abi.py` with `pyelftools` and `capstone`. Verify all imported symbols, constructor signatures, and vtable slots, then run a parked load and lifecycle test on that firmware. Static similarity alone does not establish runtime compatibility. Add a firmware entry only with matching evidence. Keep firmware approval separate from the vehicle's DME/EGS signal mapping.

For example, compare the candidate against an independently preserved reference library pair:

```sh
python3 native/tools/audit_framework_abi.py \
  --reference-dir /private/reference-frameworks \
  --installed-dir /private/candidate-frameworks \
  --plugin native/out/libBoostDeck.so --report /private/boostdeck-abi-report.json
```

Repeat for ShiftDeck and MHD Maps and inspect the report's limitations. The tool compares selected instruction prefixes and vtable relocation slots. It does not simulate HMI calls or prove that a matched framework works at runtime.
