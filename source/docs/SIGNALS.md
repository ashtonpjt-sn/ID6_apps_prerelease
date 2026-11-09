# Reference signal definitions

These are the reference Gen 1 B58/340i candidates in the current source. They are not a compatibility list for other engines. Requests use HSFZ TCP port 6801 and UDS service `22` unless marked OBD. Positive UDS replies start with `62` plus the requested DID. The table describes the data bytes after that prefix.

| Display | ECU | DID or PID | Reference conversion |
| --- | --- | --- | --- |
| Boost | DME `12` | `4205` and `4201` | Two big-endian bytes each. Absolute kPa is raw ÷ 128, ambient kPa is raw ÷ 256. The collector subtracts ambient and converts to psi. |
| AFR | DME `12` | `5889` | Two bytes. Lambda is raw × 16 ÷ 65535, then converted to gasoline-equivalent AFR. |
| Ignition timing | DME `12` | `4A49` | Two signed big-endian bytes ÷ 10. The reference `FFFF` was ambiguous and stays blank. |
| Intake air | DME `12` | `580F` | One byte, raw × 0.75 − 48 °C. `FF` is rejected. |
| Transmission temperature | EGS `18` | `DA12` | Two big-endian bytes, raw − 48 °C. |
| Rail pressure | DME `12` | `56D7` | Two big-endian bytes, raw ÷ 50 bar. |
| Oil pressure | DME `12` | `586F` | Two big-endian bytes, raw ÷ 1000 bar. |
| Coolant temperature | DME `12` | `4300` | Two big-endian bytes, raw ÷ 10 − 273.15 °C. |
| ShiftDeck RPM | DME `12` | OBD `01 0C` | Two big-endian bytes ÷ 4. |
| ShiftDeck speed | DME `12` | OBD `01 0D` | One byte in km/h, displayed in mph. |

The worker enforces data length, plausibility limits, and freshness. Look at the code for exact bounds and units. If the DME reports coolant via `01 05`, confirm that reading against the intended coolant channel before adding an explicit fallback. An OBD PID and a BMW DID are distinct requests with distinct reply formats. [Request table](../native/src/collector.c) · [Decoder](../native/src/gauge_core.c)

## More vitals candidates

The BoostDeck **More vitals** page and the Warm-Up app use these. Unlike the table above, none of them has been checked on the reference car. The worker tries each source in order and keeps the first one that answers plausibly. All are requested from the DME (`12`).

| Display | Sources, in order | Conversion and plausible range |
| --- | --- | --- |
| Engine oil | OBD `01 5C`, DID `F45C` | One byte − 40 °C, −40 to 170 °C |
| Low-pressure fuel | DID `F40A`, DID `F422`, OBD `01 0A`, OBD `01 22` | `0A`: one byte × 0.03 bar. `22`: two bytes × 0.00079 bar. 0.5 to 8 or 15 bar |
| Battery | OBD `01 42` | Two bytes ÷ 1000 V, 6 to 18 V |
| Fuel trims, bank 1 | OBD `01 06` (short), `01 07` (long) | One byte × 100 ÷ 128 − 100 % |
| Ethanol content | OBD `01 52` | One byte × 100 ÷ 255 % |
| Engine load | OBD `01 04` | One byte × 100 ÷ 255 % |
| Misfires, cylinders 1–6 | OBD `06 A2` … `06 A7`, test `0C` | Misfire count in the current driving cycle |
| Road speed (Performance Timer, clear-code check) | OBD `01 0D` | One byte, km/h |

[Warm-Up, Performance Timer, Fault Scanner and gear details](NEW_APPS.md)
