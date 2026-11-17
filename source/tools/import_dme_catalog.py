"""Build the read-only probe catalog from idrive7-apps' B58 engine data.

idrive7-apps (MIT, see third-party/idrive7-apps-LICENSE) carries a decoded copy
of the DME8FF_R description used by B58/B48/B38 engine units, with English
labels, and the DME/EGS candidate lists its M Vitals app tries in order. This
turns both into one flat list of explicit read requests that
probe_signals.py --catalog can send while parked and decode for a sanity check.

    python3 tools/import_dme_catalog.py PATH/TO/idrive7-apps tools/b58_catalog.json

Only UDS 0x22 identifiers and OBD service 01 PIDs are taken, for the DME (0x12)
and the gearbox unit (0x18). Every request is validated by probe_signals.py
again when the catalog is loaded.
"""
import argparse
import json
from pathlib import Path

SIZES = {1: "u8", 2: "u16", 4: "u32"}
# M Vitals gives ECU addresses in decimal.
VITALS_ECUS = {18: "12", 24: "18"}


def field(source, english, fallback):
    out = {"name": source["n"], "type": source["t"], "at": source["at"]}
    if "k" in source:
        out["factor"] = source["k"]
    if "o" in source:
        out["offset"] = source["o"]
    if "u" in source:
        out["unit"] = source["u"]
    if "e" in source:
        out["enum"] = source["e"]
    out["label"] = english.get(source["d"], source["d"]) if "d" in source else fallback
    return out


def catalog_requests(builtin):
    data = json.loads((builtin / "DME8FF_R.json").read_text(encoding="utf-8"))
    english = json.loads((builtin / "DME8FF_R.en.json").read_text(encoding="utf-8"))
    meta = json.loads((builtin / "DME8FF_R.meta.json").read_text(encoding="utf-8"))
    for did in meta.get("order", sorted(data)):
        entry = data[did]
        name = english.get(entry["d"], entry["d"])
        # A field without its own description takes the identifier's, or its raw name when
        # the identifier holds several values.
        fallback = (lambda f: name) if len(entry["f"]) == 1 else (lambda f: f["n"])
        yield {"ecu": "12", "request": f"22{did}".upper(), "group": "DME8FF_R",
               "job": entry["a"], "name": name,
               "fields": [field(f, english, fallback(f)) for f in entry["f"]]}, meta.get("enums", {})


def vitals_requests(app_json):
    dme = json.loads(app_json.read_text(encoding="utf-8"))["config"]["dme"]
    default_ecu = dme.get("ecu", 18)
    for group, candidates in dme.items():
        if not isinstance(candidates, list):
            continue  # comment, ecu, and the OBD service 06 misfire settings
        for candidate in candidates:
            ecu = VITALS_ECUS.get(candidate.get("ecu", default_ecu))
            if ecu is None:
                continue
            if "did" in candidate:
                request = f"22{candidate['did']}".upper()
            elif "obd" in candidate:
                request = f"01{candidate['obd']}".upper()
            else:
                continue
            size = candidate.get("bytes", 1)
            kind = SIZES[size] if not candidate.get("signed") else SIZES[size].replace("u", "s")
            out = {"name": f"{group}", "type": kind, "at": candidate.get("at", 0),
                   "factor": candidate.get("factor", 1), "offset": candidate.get("offset", 0),
                   "label": candidate.get("note", group)}
            for bound in ("min", "max"):
                if bound in candidate:
                    out[bound] = candidate[bound]
            yield {"ecu": ecu, "request": request, "group": "M Vitals", "name": group,
                   "fields": [out]}


def build(root):
    root = Path(root)
    merged, enums = {}, {}
    for request, catalog_enums in catalog_requests(root / "apps/live_data/builtin"):
        merged[(request["ecu"], request["request"])] = request
        enums.update(catalog_enums)
    for request in vitals_requests(root / "apps/car_vitals/app.json"):
        key = (request["ecu"], request["request"])
        if key in merged:
            # One reply can carry several values (PID 0x70 holds target and actual boost).
            known = {(f["name"], f["at"]) for f in merged[key]["fields"]}
            merged[key]["fields"] += [f for f in request["fields"] if (f["name"], f["at"]) not in known]
            if merged[key]["group"] != request["group"] and request["name"] not in merged[key]["name"]:
                merged[key]["name"] += f" / {request['name']}"
        else:
            merged[key] = request
    return {
        "source": "cauliflowermossgrape/idrive7-apps: apps/live_data/builtin/DME8FF_R "
                  "(SGBD DME8FF_R rev 55.000) and apps/car_vitals/app.json. MIT, see "
                  "third-party/idrive7-apps-LICENSE. Candidates, not values verified on a car.",
        "enums": enums,
        "requests": list(merged.values()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("idrive7_apps", type=Path, help="local checkout of idrive7-apps")
    parser.add_argument("out", type=Path, help="catalog JSON to write")
    args = parser.parse_args()
    catalog = build(args.idrive7_apps)
    args.out.write_text(json.dumps(catalog, indent=1, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    ecus = sorted({r["ecu"] for r in catalog["requests"]})
    print(f"{len(catalog['requests'])} requests for ECUs {', '.join(ecus)} -> {args.out}")


if __name__ == "__main__":
    main()
