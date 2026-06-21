#!/usr/bin/env python3
"""
Validate an Eternity II piece data file.

Format expected:
    ntiles: N
    <id> <top> <right> <bottom> <left>
    ...  (N lines)

Color 0 is the border (grey edge). Non-zero colors are puzzle figures.

Usage:
    python3 tools/validate_pieces.py data/pieces16.csv [data/pieces.csv ...]
"""

import sys
import math
from collections import Counter
from pathlib import Path


# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

SIDES = ("top", "right", "bottom", "left")
# Opposite side pairs: (top↔bottom, right↔left)
OPPOSITE = {0: 2, 2: 0, 1: 3, 3: 1}
# Two sides are "adjacent" when they are NOT opposite
ADJACENT_PAIRS = [(0, 1), (1, 2), (2, 3), (3, 0)]  # (top,right) (right,bottom) etc.


def is_perfect_square(n: int) -> int | None:
    """Return sqrt(n) if n is a perfect square, else None."""
    if n <= 0:
        return None
    k = int(math.isqrt(n))
    return k if k * k == n else None


# ──────────────────────────────────────────────────────────────────────────────
# Parser
# ──────────────────────────────────────────────────────────────────────────────

def parse_file(path: str) -> tuple[int, list[tuple[int, int, int, int, int]]]:
    """
    Parse the CSV file.
    Returns (declared_ntiles, pieces) where each piece is
    (id, top, right, bottom, left).
    Raises ValueError with a descriptive message on any format error.
    """
    lines = Path(path).read_text().splitlines()
    if not lines:
        raise ValueError("File is empty.")

    # Header
    header = lines[0].strip()
    if not header.startswith("ntiles:"):
        raise ValueError(f"First line must be 'ntiles: N', got: {header!r}")
    try:
        declared = int(header.split(":")[1].strip())
    except (IndexError, ValueError):
        raise ValueError(f"Cannot parse ntiles from header: {header!r}")
    if declared <= 0:
        raise ValueError(f"ntiles must be positive, got {declared}.")

    pieces: list[tuple[int, int, int, int, int]] = []
    for lineno, raw in enumerate(lines[1:], start=2):
        raw = raw.strip()
        if not raw:
            continue  # skip blank lines
        tokens = raw.split()
        if len(tokens) != 5:
            raise ValueError(
                f"Line {lineno}: expected 5 values (id top right bottom left), "
                f"got {len(tokens)}: {raw!r}"
            )
        try:
            vals = tuple(int(t) for t in tokens)
        except ValueError:
            raise ValueError(f"Line {lineno}: non-integer value in {raw!r}")
        pieces.append(vals)  # type: ignore[arg-type]

    return declared, pieces


# ──────────────────────────────────────────────────────────────────────────────
# Validation checks
# ──────────────────────────────────────────────────────────────────────────────

class Report:
    def __init__(self, path: str) -> None:
        self.path = path
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)

    def ok(self) -> bool:
        return not self.errors


def check_header_vs_data(declared: int, pieces: list, r: Report) -> None:
    actual = len(pieces)
    if actual != declared:
        r.error(
            f"Header says ntiles={declared} but file contains {actual} piece lines."
        )


def check_square(n: int, r: Report) -> int | None:
    k = is_perfect_square(n)
    if k is None:
        r.error(
            f"{n} is not a perfect square — cannot form a square board."
        )
    return k


def check_ids(pieces: list, n: int, r: Report) -> None:
    ids = [p[0] for p in pieces]
    seen: set[int] = set()
    duplicates: list[int] = []
    for pid in ids:
        if pid in seen:
            duplicates.append(pid)
        seen.add(pid)
    if duplicates:
        r.error(f"Duplicate piece IDs: {sorted(set(duplicates))}")

    expected = set(range(1, n + 1))
    missing = expected - seen
    extra = seen - expected
    if missing:
        r.error(f"Missing piece IDs: {sorted(missing)}")
    if extra:
        r.error(f"Unexpected piece IDs (out of range 1–{n}): {sorted(extra)}")


def check_face_values(pieces: list, r: Report) -> None:
    for pid, top, right, bottom, left in pieces:
        faces = (top, right, bottom, left)
        for name, val in zip(SIDES, faces):
            if val < 0:
                r.error(f"Piece {pid}: negative color on {name} ({val}).")


def check_topology(pieces: list, k: int, r: Report) -> None:
    """
    Check corner / edge / inner piece counts and zero-side placement.

    For a k×k board:
      - corners : 4   pieces with exactly 2 adjacent zero-sides
      - edges   : 4*(k-2) pieces with exactly 1 zero-side
      - inner   : (k-2)^2 pieces with no zero-side
    """
    expected_corners = 4
    expected_edges   = 4 * (k - 2)
    expected_inner   = (k - 2) ** 2

    corners = inner = edges = 0
    degenerate: list[str] = []

    for pid, top, right, bottom, left in pieces:
        faces = [top, right, bottom, left]
        zero_indices = [i for i, v in enumerate(faces) if v == 0]
        nz = len(zero_indices)

        if nz == 0:
            inner += 1
        elif nz == 1:
            edges += 1
        elif nz == 2:
            i, j = zero_indices
            if OPPOSITE[i] == j:
                # opposite sides both 0 — invalid geometry
                degenerate.append(
                    f"Piece {pid}: zero-sides on opposite faces "
                    f"({SIDES[i]} and {SIDES[j]}) — invalid for any board position."
                )
            else:
                corners += 1
        elif nz == 3:
            degenerate.append(
                f"Piece {pid}: three zero-sides — cannot fit any board position."
            )
        else:  # nz == 4
            degenerate.append(
                f"Piece {pid}: all four sides are zero — completely blank piece."
            )

    for msg in degenerate:
        r.error(msg)

    if corners != expected_corners:
        r.error(
            f"Expected {expected_corners} corner pieces (2 adjacent zero-sides), "
            f"found {corners}."
        )
    if edges != expected_edges:
        r.error(
            f"Expected {expected_edges} edge pieces (1 zero-side) for a {k}×{k} board, "
            f"found {edges}."
        )
    if inner != expected_inner:
        r.error(
            f"Expected {expected_inner} inner pieces (no zero-side), found {inner}."
        )


def check_color_pairing(pieces: list, r: Report) -> None:
    """
    For each non-zero color c:
      1. Total occurrences across all pieces must be even.
         (Each internal edge pairs two different piece-sides of the same color.)
      2. No single piece may contribute more occurrences of c than all other
         pieces combined (otherwise that piece can never be fully matched).

    Note: two occurrences of the same color on a single piece are NOT a valid
    pair — pairing requires two DIFFERENT pieces.
    """
    # total_count[c] = total occurrences of color c across all pieces
    total_count: Counter[int] = Counter()
    # per_piece[pid][c] = occurrences of c on piece pid
    per_piece: dict[int, Counter[int]] = {}

    for pid, top, right, bottom, left in pieces:
        faces = [top, right, bottom, left]
        cnt: Counter[int] = Counter(v for v in faces if v != 0)
        per_piece[pid] = cnt
        total_count.update(cnt)

    odd_colors = sorted(c for c, n in total_count.items() if n % 2 != 0)
    if odd_colors:
        for c in odd_colors:
            r.error(
                f"Color {c} appears {total_count[c]} times (odd) — "
                f"cannot be fully paired across different pieces."
            )

    # Check that no single piece monopolises more than half of any color.
    # (Necessary condition for a cross-piece perfect matching.)
    for pid, cnt in per_piece.items():
        for c, piece_count in cnt.items():
            total = total_count[c]
            others = total - piece_count
            if piece_count > others:
                r.error(
                    f"Color {c}: piece {pid} holds {piece_count} of {total} "
                    f"occurrences — more than all other pieces combined ({others}). "
                    f"Some sides of this piece can never be matched."
                )


def check_color_range(pieces: list, r: Report) -> None:
    """
    Non-zero colors should form a contiguous range [1, max_color] with no gaps,
    otherwise a gap likely signals a typo in the data file.
    """
    all_colors: set[int] = set()
    for pid, top, right, bottom, left in pieces:
        for v in (top, right, bottom, left):
            if v != 0:
                all_colors.add(v)

    if not all_colors:
        r.warn("No non-zero colors found — all faces are border (0).")
        return

    max_c = max(all_colors)
    expected = set(range(1, max_c + 1))
    gaps = expected - all_colors
    if gaps:
        r.warn(
            f"Color range has gaps — colors present: {sorted(all_colors)}, "
            f"missing values in [1,{max_c}]: {sorted(gaps)}. "
            f"This may indicate a typo in the data file."
        )

    r_info = f"Colors used: {len(all_colors)} distinct non-zero values in [1,{max_c}]."
    return r_info  # returned but not used — just for the summary print


def check_duplicate_pieces(pieces: list, r: Report) -> None:
    """
    Detect pieces that are identical (same unordered multiset of face colors),
    including all 4 rotations. Two pieces that are rotations of each other are
    identical in content.
    """
    def rotations(faces: tuple[int, int, int, int]) -> list[tuple[int, ...]]:
        t, ri, b, l = faces
        return [
            (t, ri, b, l),
            (l, t, ri, b),
            (b, l, t, ri),
            (ri, b, l, t),
        ]

    canonical: dict[tuple, int] = {}  # canonical_form → first pid
    duplicates: list[str] = []

    for pid, top, right, bottom, left in pieces:
        faces = (top, right, bottom, left)
        canon = min(rotations(faces))  # lexicographically smallest rotation
        if canon in canonical:
            duplicates.append(
                f"Piece {pid} is a rotation of piece {canonical[canon]} "
                f"(faces {faces})."
            )
        else:
            canonical[canon] = pid

    for msg in duplicates:
        r.warn(msg)


# ──────────────────────────────────────────────────────────────────────────────
# Summary statistics
# ──────────────────────────────────────────────────────────────────────────────

def print_summary(pieces: list, k: int | None) -> None:
    all_colors: Counter[int] = Counter()
    for pid, top, right, bottom, left in pieces:
        for v in (top, right, bottom, left):
            if v != 0:
                all_colors[v] += 1

    print(f"  Pieces         : {len(pieces)}")
    if k:
        print(f"  Board size     : {k}×{k}")
    print(f"  Distinct colors: {len(all_colors)} non-zero values")
    if all_colors:
        print(f"  Color range    : [1, {max(all_colors)}]")
        top5 = all_colors.most_common(5)
        print(f"  Most frequent  : {', '.join(f'{c}×{n}' for c, n in top5)}")


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

def validate(path: str) -> bool:
    r = Report(path)
    print(f"\n{'═'*60}")
    print(f"Validating: {path}")
    print(f"{'═'*60}")

    # Parse
    try:
        declared, pieces = parse_file(path)
    except (OSError, ValueError) as exc:
        print(f"  [PARSE ERROR] {exc}")
        return False

    # Run all checks
    check_header_vs_data(declared, pieces, r)
    k = check_square(declared, r)
    check_ids(pieces, declared, r)
    check_face_values(pieces, r)

    if k is not None:
        check_topology(pieces, k, r)
    else:
        r.warn("Skipping topology checks (ntiles is not a perfect square).")

    check_color_pairing(pieces, r)
    check_color_range(pieces, r)
    check_duplicate_pieces(pieces, r)

    # Summary stats
    print_summary(pieces, k)

    # Results
    if r.warnings:
        print(f"\n  Warnings ({len(r.warnings)}):")
        for w in r.warnings:
            print(f"    ⚠  {w}")

    if r.errors:
        print(f"\n  Errors ({len(r.errors)}):")
        for e in r.errors:
            print(f"    ✗  {e}")
        print(f"\n  Result: INVALID — {len(r.errors)} error(s) found.")
        return False
    else:
        status = "VALID" + (f" (with {len(r.warnings)} warning(s))" if r.warnings else "")
        print(f"\n  Result: {status}")
        return True


def main() -> None:
    paths = sys.argv[1:]
    if not paths:
        print(f"Usage: {sys.argv[0]} <pieces_file.csv> [...]")
        sys.exit(1)

    all_ok = True
    for p in paths:
        ok = validate(p)
        if not ok:
            all_ok = False

    print()
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
