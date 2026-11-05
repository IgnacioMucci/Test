#!/usr/bin/env python3
"""FlareSense full-JSON → bare-body encoder. See README.md for protocol docs.

Takes the human-readable JSON object (the same shape the decoder produces,
plus transport metadata the firmware carries) and emits the compact
binary-safe string:

    NNN,DDDDD:(ID)[v1,v2;v3]:[|TTTTTTTTTT]

Grouping rule (inverse of the decoder): the decoder OVERWRITES colliding
field names, it never merges arrays. Therefore one field's whole value array
always belongs to a single sensor block. Fields are partitioned into sensors
by schema match, most-fields-first; a field's array is never split.
"""

import argparse
import json
import sys

# ── Sensor schema ─────────────────────────────────────────────────────────────
# Same table as decode_flaresense.py — ID → (sensor_name, [ordered field names])

SENSOR_SCHEMA: dict[int, tuple[str, list[str]]] = {
    0x01: ("ds18b20", ["temperature"]),
    0x02: ("sht3x", ["temperature", "humidity"]),
    0x03: (
        "weather_station",
        ["wind_speed", "wind_gust", "wind_deg", "temperature", "humidity", "pressure"],
    ),
    0x04: ("rain_gauge", ["rain"]),
    0x07: ("bh1750", ["lux"]),
    0x09: ("battery", ["battery"]),
    0x82: ("ds18b20_aux", ["temperature"]),
}

# Alternate JSON field names for the SAME sensor id, used when another
# selected sensor already owns the primary names (README Sensor ID table,
# '||' column): weather_station + sht3x -> sht3x uploads _aux fields.
SENSOR_SCHEMA_AUX: dict[int, list[str]] = {
    0x01: ["temperature_aux"],
    0x02: ["temperature_aux", "humidity_aux"],
}

# Candidate blocks for greedy assignment: (sensor_id, fields, is_aux).
# Primary variants sort before aux variants of the same sensor.
# Widest field set first, then by sensor ID.
_SENSORS_BY_SIZE: list[tuple[int, list[str], bool]] = sorted(
    [
        (sid, fields, False)
        for sid, (_name, fields) in SENSOR_SCHEMA.items()
    ]
    + [
        (sid, fields, True)
        for sid, fields in SENSOR_SCHEMA_AUX.items()
    ],
    key=lambda t: (-len(t[1]), t[0], t[2]),
)

# ── Number formatting ────────────────────────────────────────────────────────


def _fmt_number(value: float | int | str) -> str:
    """
    Serialize one reading value using the shortest round-trip form.

    26 -> '26', 30.2 -> '30.2', 24.0 -> '24', NaN/None -> 'nan'.

    Args:
        value: numeric (or 'nan'/None) reading value.

    Returns:
        ASCII string safe for the bare-body payload.

    Raises:
        ValueError: non-numeric input.
    """
    if value is None:
        return "nan"
    if isinstance(value, bool):
        raise ValueError(f"Invalid reading value: {value!r} (bool is not numeric)")
    if isinstance(value, str):
        if value.lower() == "nan":
            return "nan"
        raise ValueError(f"Invalid reading value: {value!r} (expected number)")
    if isinstance(value, (int, float)):
        f = float(value)
        if f != f:  # NaN
            return "nan"
        if f == int(f) and abs(f) < 1e16:
            return str(int(f))
        s = repr(f)  # shortest round-trip repr in Python 3
        if "e" in s or "E" in s:
            raise ValueError(
                f"Value {f!r} needs exponent notation; bare body has no exponent form"
            )
        return s
    raise ValueError(f"Invalid reading value: {value!r} (expected number)")


# ── Sensor grouping ──────────────────────────────────────────────────────────


def _group_readings(readings: dict) -> list[tuple[int, list[str]]]:
    """
    Partition a flat readings map into ordered sensor blocks.

    Each sensor id may match its primary field names or its _aux alternate
    (README Sensor ID table '||' column); a sensor id is emitted at most once.
    Greedy pass 1: variants whose ENTIRE field set is present (widest first,
    primary before aux). Greedy pass 2: variants matching any remaining field;
    absent fields are skipped inside the block (warning emitted).

    Args:
        readings: flat {field_name: [values...]} map.

    Returns:
        List of (sensor_id, [encoded_field_strings]) in first-appearance order.
        Each encoded field string is already comma-joined values.

    Raises:
        ValueError: empty readings, or fields that match no sensor schema.
    """
    if not isinstance(readings, dict) or not readings:
        raise ValueError("'readings' must be a non-empty object")

    remaining = dict(readings)  # insertion order preserved (Python 3.7+)
    for field, values in remaining.items():
        if not isinstance(values, list) or not values:
            raise ValueError(
                f"readings.{field} must be a non-empty array of numbers"
            )

    assigned: list[tuple[int, list[str], int]] = []  # (sensor_id, fields, order)
    used: set[int] = set()  # a sensor id is emitted at most once

    # Pass 1 — full field-set match, widest variant first (primary before aux).
    for sid, fields, _is_aux in _SENSORS_BY_SIZE:
        if sid in used:
            continue
        if all(f in remaining for f in fields):
            order = min(list(remaining).index(f) for f in fields)
            for f in fields:
                del remaining[f]
            used.add(sid)
            assigned.append((sid, fields, order))

    # Pass 2 — partial match for leftovers (never happens for clean data).
    for sid, fields, _is_aux in _SENSORS_BY_SIZE:
        if sid in used:
            continue
        present = [f for f in fields if f in remaining]
        if not present:
            continue
        order = min(list(remaining).index(f) for f in present)
        for f in present:
            del remaining[f]
        used.add(sid)
        print(
            f"Warning: sensor {SENSOR_SCHEMA[sid][0]} (id {sid}) encoded with "
            f"partial field set {present}; values for absent fields are omitted.",
            file=sys.stderr,
        )
        assigned.append((sid, present, order))

    if remaining:
        raise ValueError(
            f"Fields match no sensor schema: {sorted(remaining)}; "
            f"known fields: {sorted({f for _, fl, _a in _SENSORS_BY_SIZE for f in fl})}"
        )

    assigned.sort(key=lambda t: t[2])  # first-appearance order

    blocks: list[tuple[int, list[str]]] = []
    for sid, fields, _ in assigned:
        encoded_fields: list[str] = []
        for f in fields:
            if f not in readings:
                continue
            encoded_fields.append(",".join(_fmt_number(v) for v in readings[f]))
        blocks.append((sid, encoded_fields))
    return blocks


# ── Encoder ───────────────────────────────────────────────────────────────────


def _node_id(raw: object) -> str:
    """Return the 3-char node id (last 3 chars of a full board_id)."""
    s = str(raw)
    if len(s) < 3:
        raise ValueError(f"nodeId must be at least 3 chars, got {s!r}")
    return s[-3:]


def encode(obj: dict) -> str:
    """
    Encode a full FlareSense JSON object into the bare-body string.

    Accepts both decoder key names ('delta_t') and firmware/transport names
    ('timeDeltaM'). 'fw', 'com_mode', 'rssi' and similar metadata are dropped.
    Optional 'when' (10-digit unix-seconds) is appended after the terminator
    as '|TTTTTTTTTT'.

    Args:
        obj: full JSON object with nodeId, delta_t/timeDeltaM, readings.

    Returns:
        Bare-body encoded string, e.g. "bc8,01800:(9)[35](3)[26;...]:".

    Raises:
        ValueError: missing/invalid header fields or unassignable readings.
    """
    if not isinstance(obj, dict):
        raise ValueError("Input must be a JSON object")

    if "nodeId" not in obj:
        raise ValueError("Missing required field 'nodeId'")
    node = _node_id(obj["nodeId"])

    delta_raw = obj["delta_t"] if "delta_t" in obj else obj.get("timeDeltaM")
    if delta_raw is None:
        raise ValueError("Missing 'delta_t' (or 'timeDeltaM')")
    try:
        delta_t = int(delta_raw)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"Invalid delta_t: {delta_raw!r}") from exc
    if not 0 <= delta_t <= 99999:
        raise ValueError(f"delta_t {delta_t} out of 5-digit range (0..99999)")

    blocks = _group_readings(obj.get("readings", {}))

    body = "".join(
        f"({sid})[{';'.join(fields)}]" for sid, fields in blocks
    )

    out = f"{node},{delta_t:05d}:{body}:"

    when = obj.get("when")
    if when is not None:
        try:
            when_i = int(when)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"Invalid when timestamp: {when!r}") from exc
        if not 0 <= when_i <= 9999999999:
            raise ValueError(f"when {when_i} does not fit 10 ASCII digits")
        out += f"|{when_i:010d}"

    return out


# ── CLI ───────────────────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Encode a full FlareSense JSON object into the bare-body format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="-",
        help="JSON string, path to a .json file, or '-' (default) for stdin.",
    )
    args = parser.parse_args()

    src = args.input.strip()
    if src == "-":
        raw_json = sys.stdin.read()
    elif src.startswith(("{", "[")):
        raw_json = src  # inline JSON literal
    else:
        try:
            with open(src, "r", encoding="utf-8") as f:
                raw_json = f.read()
        except OSError as exc:
            print(f"Error: cannot read input or parse as JSON: {exc}", file=sys.stderr)
            sys.exit(1)

    try:
        obj = json.loads(raw_json)
    except json.JSONDecodeError as exc:
        print(f"Error: invalid JSON input: {exc}", file=sys.stderr)
        sys.exit(1)

    try:
        encoded = encode(obj)
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)

    print(encoded)


if __name__ == "__main__":
    main()