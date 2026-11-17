#!/usr/bin/env python3
"""Read explicit BMW diagnostic signals over wired HSFZ while parked."""

import argparse
from ipaddress import IPv4Address
import json
import re
import socket
import struct
import time


def enet_address(value):
    address = IPv4Address(value)
    if not address.is_link_local or address.packed[2] in (0, 255):
        raise argparse.ArgumentTypeError("use a usable 169.254.x.x ENET address")
    return str(address)


def read_request(value):
    if not re.fullmatch(r"01[0-9a-fA-F]{2}|22[0-9a-fA-F]{4}", value):
        raise argparse.ArgumentTypeError("use a read-only OBD PID (0105) or UDS DID (224300)")
    return bytes.fromhex(value)


CATALOG_LIMIT = 200
TYPES = {"u8": (1, False), "s8": (1, True), "u16": (2, False), "s16": (2, True), "u32": (4, False), "s32": (4, True)}


def load_catalog(path, only=()):
    """Return (ecu, request bytes, entry) for each catalog read, validated like explicit requests."""
    with open(path, encoding="utf-8") as handle:
        catalog = json.load(handle)
    wanted = {value.lower() for value in only}
    plan = []
    for entry in catalog["requests"]:
        if entry.get("ecu") not in ("12", "18"):
            raise argparse.ArgumentTypeError(f"catalog ECU {entry.get('ecu')!r} is not the DME (12) or EGS (18)")
        request = read_request(entry["request"])
        if wanted and entry["request"].lower() not in wanted:
            continue
        for field in entry.get("fields", ()):
            if field.get("type") not in TYPES:
                raise argparse.ArgumentTypeError(f"catalog type {field.get('type')!r} for {entry['request']}")
        plan.append((entry["ecu"], request, entry))
    if len(plan) > CATALOG_LIMIT:
        raise argparse.ArgumentTypeError(f"catalog has {len(plan)} reads; the limit is {CATALOG_LIMIT}")
    return plan, catalog.get("enums", {})


def decode(fields, data, enums=None):
    """Decode big-endian fields as raw * factor + offset; flag short replies and values out of bounds."""
    values = {}
    for field in fields:
        size, signed = TYPES[field["type"]]
        at = field.get("at", 0)
        label = field.get("label", field["name"])
        if at + size > len(data):
            values[label] = "short reply"
            continue
        raw = int.from_bytes(data[at:at + size], "big", signed=signed)
        if "enum" in field:
            values[label] = (enums or {}).get(field["enum"], {}).get(str(raw), raw)
            continue
        value = round(raw * field.get("factor", 1) + field.get("offset", 0), 4)
        if not field.get("min", value) <= value <= field.get("max", value):
            value = f"{value} (outside {field.get('min', '')}..{field.get('max', '')})"
        elif field.get("unit"):
            value = f"{value} {field['unit']}"
        values[label] = value
    return values


def exact(connection, length, deadline):
    data = bytearray()
    while len(data) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("no matching ECU response")
        connection.settimeout(remaining)
        part = connection.recv(length - len(data))
        if not part:
            raise ConnectionError("gateway closed the connection")
        data.extend(part)
    return data


def exchange(connection, ecu, request):
    connection.settimeout(2)
    connection.sendall(struct.pack(">IHBB", len(request) + 2, 1, 0xF4, ecu) + request)
    deadline = time.monotonic() + 2
    prefix = bytes((request[0] + 0x40,)) + request[1:]
    while True:
        length, control = struct.unpack(">IH", exact(connection, 6, deadline))
        if length > 8192:
            raise ValueError("oversized HSFZ frame")
        payload = exact(connection, length, deadline)
        if control == 0x12:
            connection.sendall(struct.pack(">IHBB", 2, 0x13, 0, 0xF4))
        if control != 1 or payload[:2] != bytes((ecu, 0xF4)):
            continue
        reply = bytes(payload[2:])
        if reply.startswith(prefix):
            return {"reply": reply.hex(" "), "data": reply[len(prefix):].hex(" ")}
        if len(reply) == 3 and reply[:2] == bytes((0x7F, request[0])) and reply[2] != 0x78:
            return {"reply": reply.hex(" "), "nrc": f"{reply[2]:02X}"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local", required=True, type=enet_address, help="computer's wired ENET IPv4")
    parser.add_argument("--gateway", required=True, type=enet_address, help="diagnostic gateway IPv4")
    parser.add_argument("--ecu", choices=("12", "18"), default="12", help="12 for DME or 18 for EGS")
    parser.add_argument("--catalog", help="JSON list of read-only requests to survey, e.g. tools/b58_catalog.json")
    parser.add_argument("--only", default="", help="with --catalog: comma-separated requests to keep, e.g. 224201,22f470")
    parser.add_argument("requests", nargs="*", type=read_request)
    args = parser.parse_args()
    if args.local == args.gateway or len(args.requests) > 12:
        parser.error("use separate local and gateway IPs and at most 12 explicit requests")
    enums = {}
    if args.catalog:
        if args.requests:
            parser.error("use either --catalog or explicit requests")
        try:
            plan, enums = load_catalog(args.catalog, [v for v in args.only.split(",") if v])
        except (OSError, ValueError, KeyError, argparse.ArgumentTypeError) as error:
            parser.error(f"catalog: {error}")
        if not plan:
            parser.error("no catalog reads match --only")
    elif args.requests:
        plan = [(args.ecu, request, None) for request in args.requests]
    else:
        parser.error("give explicit requests or --catalog")
    try:
        for ecu, request, entry in plan:
            result = {"ecu": ecu, "request": request.hex(" ")}
            if entry:
                result["name"] = entry.get("name", "")
            try:
                # A separate connection prevents a late response from matching a later read.
                with socket.create_connection((args.gateway, 6801), timeout=2, source_address=(args.local, 0)) as connection:
                    result.update(exchange(connection, int(ecu, 16), request))
            except TimeoutError as error:
                result["error"] = str(error)
            if entry and "data" in result:
                result["decoded"] = decode(entry.get("fields", ()), bytes.fromhex(result["data"]), enums)
            print(json.dumps(result), flush=True)
            time.sleep(0.2)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from None


if __name__ == "__main__":
    main()
