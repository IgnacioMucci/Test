#!/usr/bin/env python3
"""CSV samples → full FlareSense JSON (the backwards process).

The CSV must carry a header row with the column names, e.g.:

    "deviceId","temperature","temperature_aux","humidity","humidity_aux",
    "pressure","wind_direction","wind_gust","wind_speed","battery"

Column order and reading names come from the header. Only two aliases are
applied: the id column becomes nodeId, and wind_direction becomes wind_deg
(the protocol name used by the full JSON / bare-body schema).

Grabbing X lines produces ONE JSON object with a single nodeId and one array
per sensor holding X values (index-aligned across sensors) — the full-JSON
shape that feeds encode_flaresense.py.

Missing metadata (fw, timeDeltaM, com_mode, rssi) is filled from TEMPLATE.
The firmware invalid marker -999.9 becomes null.
"""

import argparse
import csv
import json
import sys

# Header name → JSON key aliases. Case-insensitive on the header side.
# Any other header name is used verbatim as the readings key.
ID_ALIASES = {"deviceid", "nodeid", "boardid", "board_id"}
KEY_ALIASES = {"wind_direction": "wind_deg"}

# Fields not carried in the CSV — filled from the reference full-JSON sample.
TEMPLATE: dict = {
    "fw": "v3.4.681-development-89d6deb",
    "timeDeltaM": 1800,
    "com_mode": 0,
    "rssi": 215,
}

# Firmware marker for an invalid/failed sensor reading.
SENTINEL = -999.9


# ── Value conversion ─────────────────────────────────────────────────────────


def _is_number(text: str) -> bool:
    """Return True if the cell parses as a float."""
    try:
        float(text)
        return True
    except ValueError:
        return False


def _to_number(raw: str) -> float | int | None:
    """
    Convert one CSV cell to a JSON number, or None for the invalid marker.

    5.5 -> 5.5, 4074 -> 4074 (int), 1011.0 -> 1011 (int), -999.9 -> None.

    Args:
        raw: cell text.

    Returns:
        int/float value, or None when the sentinel is present.

    Raises:
        ValueError: cell is not numeric.
    """
    text = raw.strip()
    if not text:
        raise ValueError(f"Empty cell in CSV; expected a number or {SENTINEL}")
    value = float(text)  # raises ValueError with a clear message if not numeric
    if value == SENTINEL:
        return None
    if value == int(value):
        return int(value)
    return value


# ── Header handling ──────────────────────────────────────────────────────────


def _map_header(cells: list[str]) -> tuple[str, list[str]]:
    """
    Turn a raw header row into (id_key, [reading_key, ...]).

    Args:
        cells: header cell texts, e.g. ['deviceId', 'temperature', ...].

    Returns:
        The id column name and the ordered list of JSON readings keys after
        aliasing (wind_direction -> wind_deg; unknown names verbatim).

    Raises:
        ValueError: no id column found, or duplicate reading keys.
    """
    names = [c.strip() for c in cells]
    id_key = next((n for n in names if n.lower() in ID_ALIASES), None)
    if id_key is None:
        raise ValueError(
            f"Header has no id column (expected one of {sorted(ID_ALIASES)}); "
            f"got {names}"
        )
    keys = [KEY_ALIASES.get(n, n) for n in names if n != id_key]
    dupes = {k for k in keys if keys.count(k) > 1}
    if dupes:
        raise ValueError(f"Header maps to duplicate reading keys: {sorted(dupes)}")
    return id_key, keys


# ── Builder ───────────────────────────────────────────────────────────────────


def build_json(rows: list[list[str]], header: list[str]) -> dict:
    """
    Build one full-JSON object from aligned CSV sample rows.

    Args:
        rows: data rows (no header), each with len(header) cells.
        header: raw header cells defining column order and names.

    Returns:
        Full FlareSense JSON object: nodeId, template metadata, readings
        (each key maps to an index-aligned array with one value per row,
        in header order).

    Raises:
        ValueError: no rows, ragged row, or mixed deviceids.
    """
    id_key, keys = _map_header(header)
    id_idx = [c.strip() for c in header].index(id_key)

    if not rows:
        raise ValueError("No data rows found in CSV")

    node_id: str | None = None
    series: dict[str, list] = {key: [] for key in keys}

    for line_no, row in enumerate(rows, start=1):
        if len(row) != len(header):
            raise ValueError(
                f"Data row {line_no}: expected {len(header)} columns, got {len(row)}"
            )

        device = row[id_idx].strip()
        if node_id is None:
            node_id = device
        elif device != node_id:
            raise ValueError(
                f"Data row {line_no}: mixed deviceids ({node_id!r} vs {device!r}); "
                f"one JSON object holds one node"
            )

        for key, cell in zip(keys, (c for i, c in enumerate(row) if i != id_idx)):
            series[key].append(_to_number(cell))

    result: dict = {"nodeId": node_id}
    result.update(TEMPLATE)
    result["readings"] = {key: series[key] for key in keys}
    return result


def csv_to_json(path: str, samples: int | None, offset: int) -> dict:
    """
    Read up to `samples` data rows from `path` (after `offset` skipped).

    The first non-blank row is the header and is required.

    Args:
        path: CSV file path, or '-' for stdin.
        samples: number of sample lines to grab, or None for the whole file.
        offset: number of data rows to skip after the header.

    Returns:
        Full-JSON object (see build_json).

    Raises:
        ValueError: missing header, no rows, ragged rows, mixed deviceids.
    """
    stream = (
        open(path, "r", encoding="utf-8", newline="")
        if path != "-"
        else sys.stdin
    )

    header: list[str] | None = None
    rows: list[list[str]] = []
    try:
        for row in csv.reader(stream):
            if not row or all(not cell.strip() for cell in row):
                continue  # blank line
            if header is None:
                if _is_number(row[-1]):
                    raise ValueError(
                        "First CSV row looks like data, not a header; "
                        "add a header row with the column names"
                    )
                header = row
                continue
            if offset > 0:
                offset -= 1
                continue
            rows.append(row)
            if samples is not None and len(rows) >= samples:
                break
    finally:
        if path != "-":
            stream.close()

    if header is None:
        raise ValueError("CSV is empty; a header row is required")
    return build_json(rows, header)


# ── CLI ───────────────────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert CSV sample rows into one full FlareSense JSON object.",
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="-",
        help="CSV file path with a header row, or '-' (default) to read from stdin.",
    )
    parser.add_argument(
        "-n", "--samples",
        type=int,
        default=None,
        help="Number of sample lines to grab (default: the whole file).",
    )
    parser.add_argument(
        "--offset",
        type=int,
        default=0,
        help="Skip this many data rows after the header before collecting (default: 0).",
    )
    parser.add_argument(
        "-o", "--output",
        metavar="FILE",
        help="Write JSON to FILE instead of stdout.",
    )
    parser.add_argument(
        "--indent",
        type=int,
        default=2,
        help="JSON indentation spaces; 0 for compact (default: 2).",
    )
    args = parser.parse_args()

    try:
        obj = csv_to_json(args.input, args.samples, args.offset)
    except (ValueError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
    except csv.Error as exc:
        print(f"Error: malformed CSV: {exc}", file=sys.stderr)
        sys.exit(1)

    indent = args.indent if args.indent > 0 else None
    text = json.dumps(obj, indent=indent)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print(f"Wrote {args.output} ({len(next(iter(obj['readings'].values())))} samples)",
              file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()