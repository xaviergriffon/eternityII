#!/usr/bin/env python3
"""
Générateur de CLONES d'Eternity II à solution connue.

Pourquoi cet outil existe
-------------------------
Tous les instruments du dépôt mesurent la RÉFUTATION (`make bench-refutation`,
coût de fermeture d'un sous-arbre mort) ou le DÉBIT (`tests/bench/bench_search.sh`).
Aucun ne peut voir l'ordre des VALEURS — dans un sous-arbre mort, tous les fils
sont explorés quel que soit leur ordre, donc le compte de nœuds d'une réfutation
n'en dépend pas. Or c'est l'ordre des valeurs, et le choix de la racine, qui
décident à quel moment la branche portant la solution est atteinte. Le puzzle
réel n'ayant jamais été résolu, ce côté-là n'est mesurable que sur des instances
CONSTRUITES autour d'une solution plantée : c'est ce que produit ce script.
Voir docs/conception/banc_resolution_clones.md (§3.1) et
tests/bench/bench_solve.c, qui consomme ces clones.

Ce que le générateur garantit
-----------------------------
1. Une solution existe PAR CONSTRUCTION (le plateau est tiré d'abord, découpé
   ensuite) ; elle est écrite dans `solution_*.txt` pour contrôle.
2. L'histogramme des couleurs suit celui de `data/pieces.csv` mis à l'échelle,
   et non un uniforme naïf : chaque couleur reçoit ⌊E/k⌋ ou ⌈E/k⌉ arêtes.
   Sur le 16×16 réel cette règle redonne EXACTEMENT les comptes observés
   (12 couleurs à 25 arêtes, 5 à 24 ; cadre 12 arêtes chacune).
3. Aucune pièce dupliquée (à rotation près) et aucune pièce à symétrie de
   rotation — les deux propriétés ont été VÉRIFIÉES sur `data/pieces.csv`
   (0 doublon, 0 pièce symétrique) avant d'être imposées ici : cf. le point
   laissé ouvert en §5 du document de conception, désormais tranché.

Conventions de format
---------------------
- `pieces_*.csv` : `ntiles: N` puis `id top left bottom right`. C'est l'ordre
  que lit `read_parts` (src/core/readdata.c) — le seul qui compte, puisque
  c'est le moteur qui relit le fichier. `tools/validate_pieces.py` documente
  l'ordre `id top right bottom left` ; l'écart n'est qu'un MIROIR du plateau et
  aucun de ses contrôles (comptes de coins/bords, parité des couleurs,
  doublons à rotation près) n'y est sensible, d'où son emploi tel quel ici.
- `indices_*.csv` : `nindices: N` puis `id x y rotation mandatory`, format de
  `read_indices`. `x` est la COLONNE, `y` la LIGNE ; `top` regarde y−1 et
  `left` regarde x−1.
- Couleurs : intérieures `1..k`, cadre `k+1..k+m`, gris `0` sur le pourtour —
  la convention de `data/pieces.csv` (intérieures 1..17, cadre 18..22).
- Rotation `r` : le moteur (`rotatePart`, src/core/part.c) produit
  `tourné[s] = base[(s − r) mod 4]` avec `s` dans (top, right, bottom, left).

Usage
-----
    python3 tools/gen_clone.py --size 10 --inner-colours 9 --seed 1 --hints 5
    python3 tools/gen_clone.py --size 8 --inner-colours 6 --hints 0 --out-dir data/clones

Le binaire correspondant se compile avec la taille en dur :

    make WERROR=1 CPPFLAGS=-DETERN_PARTS=100
    ./eternityII test data/clones/pieces_10_9_1.csv \\
        --indices-file data/clones/indices_10_9_1.csv --stop-on-solution
"""

import argparse
import os
import random
import subprocess
import sys
from collections import Counter

# Index des faces dans un quadruplet, dans l'ordre du moteur.
TOP, RIGHT, BOTTOM, LEFT = 0, 1, 2, 3
SIDE_NAMES = ("top", "right", "bottom", "left")

# Tailles pour lesquelles le binaire sait être compilé (ETERN_SIZE,
# src/core/core_static_variables.h). Générer un clone d'une autre taille
# produirait un jeu de pièces que rien ne peut lire.
SUPPORTED_SIZES = (4, 8, 10, 12, 14, 16)

# Statistiques de data/pieces.csv, reproduites à l'échelle (cf. en-tête).
DEFAULT_FRAME_COLOURS = 5


# ──────────────────────────────────────────────────────────────────────────────
# Tirage du plateau
# ──────────────────────────────────────────────────────────────────────────────

def is_border(x: int, y: int, n: int) -> bool:
    """Vrai si la case (x, y) touche le pourtour du plateau."""
    return x == 0 or y == 0 or x == n - 1 or y == n - 1


def edge_slots(n: int) -> tuple[list, list]:
    """
    Énumère les arêtes INTÉRIEURES du plateau, séparées en deux familles.

    Une arête est identifiée par ('v', x, y) — entre (x,y) et (x+1,y) — ou
    ('h', x, y) — entre (x,y) et (x,y+1).

    Une arête entre DEUX cases de bordure porte une couleur de cadre : c'est
    l'anneau d'arêtes qui longe le pourtour (4(n−1) arêtes, 60 sur le 16×16).
    Toutes les autres portent une couleur intérieure.
    """
    frame, inner = [], []
    for x in range(n - 1):
        for y in range(n):
            slot = ("v", x, y)
            both_border = is_border(x, y, n) and is_border(x + 1, y, n)
            (frame if both_border else inner).append(slot)
    for x in range(n):
        for y in range(n - 1):
            slot = ("h", x, y)
            both_border = is_border(x, y, n) and is_border(x, y + 1, n)
            (frame if both_border else inner).append(slot)
    return frame, inner


def assign_balanced(slots: list, palette: list[int], rng: random.Random) -> dict:
    """
    Répartit `palette` sur `slots` de façon ÉQUILIBRÉE puis aléatoire.

    Chaque couleur reçoit ⌊len(slots)/len(palette)⌋ ou ⌈…⌉ arêtes ; quelles
    couleurs reçoivent l'unité supplémentaire est tiré au sort (sinon les
    premières couleurs de la palette seraient systématiquement les plus
    fréquentes). C'est le « pas un uniforme naïf » du §3.1 : un tirage i.i.d.
    donnerait un histogramme bien plus dispersé que celui de pieces.csv, et
    donc des tailles de compartiments (candidats par clé) sans rapport.
    """
    if not palette:
        raise ValueError("palette vide")
    order = list(palette)
    rng.shuffle(order)
    colours = [order[i % len(order)] for i in range(len(slots))]
    rng.shuffle(colours)
    return dict(zip(slots, colours))


def cell_faces(x: int, y: int, n: int, edges: dict) -> tuple[int, int, int, int]:
    """Quadruplet (top, right, bottom, left) de la case (x, y) du plateau tiré."""
    top = 0 if y == 0 else edges[("h", x, y - 1)]
    bottom = 0 if y == n - 1 else edges[("h", x, y)]
    left = 0 if x == 0 else edges[("v", x - 1, y)]
    right = 0 if x == n - 1 else edges[("v", x, y)]
    return (top, right, bottom, left)


def unrotate(oriented: tuple[int, int, int, int], r: int) -> tuple[int, ...]:
    """
    Pièce de base dont la rotation `r` redonne `oriented`.

    Inverse de `rotatePart` (src/core/part.c), qui calcule
    `tourné[s] = base[(s − r) mod 4]` : donc `base[t] = oriented[(t + r) mod 4]`.
    """
    return tuple(oriented[(t + r) % 4] for t in range(4))


def rotate(base: tuple[int, ...], r: int) -> tuple[int, ...]:
    """Applique la rotation `r` — la fonction réellement implémentée par le moteur."""
    return tuple(base[(s - r) % 4] for s in range(4))


def canonical(faces: tuple[int, ...]) -> tuple[int, ...]:
    """Forme canonique d'une pièce à rotation près (pour détecter les doublons)."""
    return min(rotate(faces, r) for r in range(4))


def has_rotational_symmetry(faces: tuple[int, ...]) -> bool:
    """Vrai si deux rotations distinctes de la pièce coïncident."""
    return len({rotate(faces, r) for r in range(4)}) < 4


class DrawRejected(Exception):
    """Tirage à rejeter (doublon ou pièce symétrique) — cf. §3.1.3."""


def draw_instance(n: int, k: int, m: int, rng: random.Random) -> tuple[dict, list, list]:
    """
    Tire un plateau complet et le découpe en pièces.

    @return (edges, board, pieces) où `board[x][y] = (piece_id, rotation)` est
            la solution plantée et `pieces[id - 1] = (id, top, right, bottom, left)`
            la pièce en orientation de base (rotation 0).
    @raise  DrawRejected si le découpage produit deux pièces identiques à
            rotation près, ou une pièce à symétrie de rotation.
    """
    frame_slots, inner_slots = edge_slots(n)
    inner_palette = list(range(1, k + 1))
    frame_palette = list(range(k + 1, k + m + 1))
    edges = {}
    edges.update(assign_balanced(frame_slots, frame_palette, rng))
    edges.update(assign_balanced(inner_slots, inner_palette, rng))

    # Découpage : une pièce par case, dans une rotation tirée au sort. La
    # rotation n'est pas cosmétique — sans elle, la rotation 0 de chaque pièce
    # serait déjà celle de la solution, et l'instance serait triviale.
    cells = [(x, y) for x in range(n) for y in range(n)]
    ids = list(range(1, n * n + 1))
    rng.shuffle(ids)  # permutation aléatoire des identifiants (§3.1.2)

    pieces = [None] * (n * n)
    board = [[None] * n for _ in range(n)]
    seen = {}
    for (x, y), pid in zip(cells, ids):
        oriented = cell_faces(x, y, n, edges)
        r = rng.randrange(4)
        base = unrotate(oriented, r)
        if has_rotational_symmetry(base):
            raise DrawRejected(f"pièce symétrique en ({x},{y}) : {base}")
        key = canonical(base)
        if key in seen:
            raise DrawRejected(f"pièces identiques en {seen[key]} et ({x},{y}) : {base}")
        seen[key] = (x, y)
        pieces[pid - 1] = (pid,) + base
        board[x][y] = (pid, r)
    return edges, board, pieces


# ──────────────────────────────────────────────────────────────────────────────
# Indices
# ──────────────────────────────────────────────────────────────────────────────

def hint_positions(n: int) -> list[tuple[int, int]]:
    """
    Les cinq positions d'indice, aux mêmes coordonnées RELATIVES que les
    officielles : les quatre « coins intérieurs » et le centre.

    Sur le 16×16 cela redonne exactement data/indices.csv : (2,2), (13,2),
    (2,13), (13,13) et (7,8) pour le centre — d'où `(n//2 - 1, n//2)`, valable
    pour toute taille paire (les seules compilables).
    """
    return [
        (n // 2 - 1, n // 2),  # centre : l'indice « mandatory »
        (2, 2),
        (n - 3, 2),
        (2, n - 3),
        (n - 3, n - 3),
    ]


def build_hints(n: int, board: list, count: int) -> list[tuple[int, int, int, int, int]]:
    """@return lignes `(id, x, y, rotation, mandatory)` prêtes pour read_indices."""
    if count == 0:
        return []
    positions = hint_positions(n)
    if len(set(positions)) != len(positions):
        raise SystemExit(
            f"--hints 5 impossible en taille {n} : les cinq positions d'indice "
            f"se recouvrent ({positions}). Utiliser --hints 0, ou une taille >= 8."
        )
    hints = []
    for i, (x, y) in enumerate(positions[:count]):
        pid, rot = board[x][y]
        hints.append((pid, x, y, rot, 1 if i == 0 else 0))
    return hints


# ──────────────────────────────────────────────────────────────────────────────
# Écriture
# ──────────────────────────────────────────────────────────────────────────────

def write_pieces(path: str, pieces: list) -> None:
    """Écrit au format read_parts : `id top left bottom right` (cf. en-tête)."""
    with open(path, "w") as f:
        f.write(f"ntiles: {len(pieces)}\n")
        for pid, top, right, bottom, left in pieces:
            f.write(f"{pid} {top} {left} {bottom} {right}\n")


def write_indices(path: str, hints: list) -> None:
    with open(path, "w") as f:
        f.write(f"nindices: {len(hints)}\n")
        for pid, x, y, rot, mandatory in hints:
            f.write(f"{pid} {x} {y} {rot} {mandatory}\n")


def write_solution(path: str, n: int, k: int, m: int, seed: int, board: list) -> None:
    """
    Écrit la solution plantée : un en-tête, la liste `x y id rotation`, puis
    une vue lisible. Sert au contrôle (`--check-solution`), jamais au moteur.
    """
    with open(path, "w") as f:
        f.write(f"# clone eternityII : size={n} inner_colours={k} "
                f"frame_colours={m} seed={seed}\n")
        f.write(f"size: {n}\n")
        for x in range(n):
            for y in range(n):
                pid, rot = board[x][y]
                f.write(f"{x} {y} {pid} {rot}\n")
        f.write("#\n# grille (ligne y, colonne x), « id/rotation » :\n")
        for y in range(n):
            row = " ".join(f"{board[x][y][0]:>4}/{board[x][y][1]}" for x in range(n))
            f.write(f"# {row}\n")


def read_solution(path: str) -> tuple[int, list]:
    """Relit un `solution_*.txt` : @return (n, board)."""
    n = None
    cells = {}
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("size:"):
                n = int(line.split(":")[1])
                continue
            x, y, pid, rot = (int(t) for t in line.split())
            cells[(x, y)] = (pid, rot)
    if n is None:
        raise ValueError(f"{path} : ligne `size:` absente")
    board = [[cells[(x, y)] for y in range(n)] for x in range(n)]
    return n, board


def read_pieces(path: str) -> list:
    """
    Relit un `pieces_*.csv` DANS L'ORDRE DU MOTEUR (`id top left bottom right`)
    et renvoie des quadruplets `(id, top, right, bottom, left)`.
    """
    pieces = []
    with open(path) as f:
        header = f.readline().strip()
        if not header.startswith("ntiles:"):
            raise ValueError(f"{path} : en-tête `ntiles:` absent")
        declared = int(header.split(":")[1])
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            pid, top, left, bottom, right = (int(t) for t in line.split())
            pieces.append((pid, top, right, bottom, left))
    if len(pieces) != declared:
        raise ValueError(f"{path} : {len(pieces)} pièces lues, {declared} annoncées")
    return pieces


# ──────────────────────────────────────────────────────────────────────────────
# Auto-contrôle (§3.1.5)
# ──────────────────────────────────────────────────────────────────────────────

def check_solution(pieces_path: str, solution_path: str, indices_path: str | None) -> list[str]:
    """
    Ré-assemble la solution DEPUIS LES FICHIERS PRODUITS et vérifie chaque arête.

    Volontairement indépendant des structures en mémoire du tirage : c'est un
    contrôle du FICHIER, pas du code qui l'a écrit. Une erreur d'ordre de
    colonnes ou de sens de rotation ne survivrait pas à ce passage.

    @return liste des erreurs (vide si tout est cohérent).
    """
    errors: list[str] = []
    pieces = {p[0]: p[1:] for p in read_pieces(pieces_path)}
    n, board = read_solution(solution_path)

    if len(pieces) != n * n:
        errors.append(f"{len(pieces)} pièces pour un plateau {n}×{n}")

    used = Counter(board[x][y][0] for x in range(n) for y in range(n))
    missing = sorted(set(pieces) - set(used))
    twice = sorted(pid for pid, c in used.items() if c > 1)
    if missing:
        errors.append(f"pièces absentes de la solution : {missing[:10]}")
    if twice:
        errors.append(f"pièces posées plusieurs fois : {twice[:10]}")

    def oriented(x, y):
        pid, rot = board[x][y]
        if pid not in pieces:
            errors.append(f"({x},{y}) : pièce {pid} inconnue")
            return (0, 0, 0, 0)
        return rotate(pieces[pid], rot)

    grid = [[oriented(x, y) for y in range(n)] for x in range(n)]
    for x in range(n):
        for y in range(n):
            f = grid[x][y]
            # Pourtour : la face tournée vers l'extérieur doit être grise, et
            # aucune face intérieure ne doit l'être.
            for side, (dx, dy) in zip((TOP, RIGHT, BOTTOM, LEFT),
                                      ((0, -1), (1, 0), (0, 1), (-1, 0))):
                nx, ny = x + dx, y + dy
                outside = not (0 <= nx < n and 0 <= ny < n)
                if outside and f[side] != 0:
                    errors.append(f"({x},{y}) face {SIDE_NAMES[side]} = {f[side]} "
                                  f"au lieu de 0 sur le pourtour")
                if not outside and f[side] == 0:
                    errors.append(f"({x},{y}) face {SIDE_NAMES[side]} grise "
                                  f"alors qu'elle touche ({nx},{ny})")
            if x + 1 < n and f[RIGHT] != grid[x + 1][y][LEFT]:
                errors.append(f"arête verticale ({x},{y})-({x+1},{y}) : "
                              f"{f[RIGHT]} != {grid[x+1][y][LEFT]}")
            if y + 1 < n and f[BOTTOM] != grid[x][y + 1][TOP]:
                errors.append(f"arête horizontale ({x},{y})-({x},{y+1}) : "
                              f"{f[BOTTOM]} != {grid[x][y+1][TOP]}")

    if indices_path is not None:
        with open(indices_path) as fh:
            head = fh.readline().strip()
            declared = int(head.split(":")[1])
            lines = [l.split() for l in fh if l.strip()]
        if len(lines) != declared:
            errors.append(f"{indices_path} : {len(lines)} lignes, {declared} annoncées")
        for tok in lines:
            pid, x, y, rot, _mandatory = (int(t) for t in tok)
            if board[x][y] != (pid, rot):
                errors.append(f"indice ({x},{y}) = pièce {pid} rotation {rot}, "
                              f"la solution y pose {board[x][y]}")
    return errors


def run_validate_pieces(path: str) -> int:
    """
    Passe le clone par tools/validate_pieces.py (§3.1.3) — le même contrôle de
    format que les jeux de pièces livrés dans le dépôt.
    """
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "validate_pieces.py")
    if not os.path.exists(script):
        print(f"validate_pieces.py introuvable ({script}) — contrôle de format ignoré",
              file=sys.stderr)
        return 0
    return subprocess.call([sys.executable, script, path])


# ──────────────────────────────────────────────────────────────────────────────
# Entrée
# ──────────────────────────────────────────────────────────────────────────────

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Génère un clone d'Eternity II à solution connue "
                    "(docs/conception/banc_resolution_clones.md).")
    ap.add_argument("--size", type=int, required=True,
                    help=f"côté du plateau ; tailles compilables : {SUPPORTED_SIZES}")
    ap.add_argument("--inner-colours", type=int, required=True,
                    help="nombre de couleurs intérieures k (17 sur le puzzle réel)")
    ap.add_argument("--frame-colours", type=int, default=DEFAULT_FRAME_COLOURS,
                    help=f"nombre de couleurs de cadre m (défaut {DEFAULT_FRAME_COLOURS})")
    ap.add_argument("--seed", type=int, default=0, help="graine du tirage (défaut 0)")
    ap.add_argument("--hints", type=int, default=0, choices=(0, 5),
                    help="nombre d'indices imposés (0 ou 5, défaut 0)")
    ap.add_argument("--out-dir", default=".", help="répertoire de sortie (défaut .)")
    ap.add_argument("--prefix", default=None,
                    help="préfixe des fichiers (défaut <n>_<k>_<seed>)")
    ap.add_argument("--max-attempts", type=int, default=200,
                    help="tirages successifs avant abandon sur doublon (défaut 200)")
    ap.add_argument("--no-validate", action="store_true",
                    help="n'appelle pas tools/validate_pieces.py")
    args = ap.parse_args(argv)

    n, k, m = args.size, args.inner_colours, args.frame_colours
    if n not in SUPPORTED_SIZES:
        print(f"taille {n} non compilable : le binaire ne connaît que "
              f"{SUPPORTED_SIZES} (ETERN_SIZE, core_static_variables.h)", file=sys.stderr)
        return 1
    if k < 1 or m < 1:
        print("--inner-colours et --frame-colours doivent être >= 1", file=sys.stderr)
        return 1
    if args.hints == 5 and n < 8:
        print(f"--hints 5 exige une taille >= 8 (les cinq positions se recouvrent "
              f"en {n}×{n})", file=sys.stderr)
        return 1
    if args.hints == 5 and n < 10:
        print(f"attention : en {n}×{n}, les positions d'indice n'ont pas le même "
              f"rôle relatif qu'en 16×16 — ne comparer « avec / sans indices » "
              f"qu'à partir de n = 10 (§5 du document de conception).", file=sys.stderr)

    frame_slots, inner_slots = edge_slots(n)
    if len(inner_slots) < k:
        print(f"{len(inner_slots)} arêtes intérieures pour {k} couleurs : "
              f"certaines n'apparaîtraient jamais", file=sys.stderr)
        return 1
    if len(frame_slots) < m:
        print(f"{len(frame_slots)} arêtes de cadre pour {m} couleurs : "
              f"certaines n'apparaîtraient jamais", file=sys.stderr)
        return 1

    # Rejet et RE-tirage (§3.1.3) : la graine reste l'entrée unique, les
    # tentatives successives en dérivent de façon déterministe.
    attempt = 0
    while True:
        attempt += 1
        if attempt > args.max_attempts:
            print(f"abandon après {args.max_attempts} tirages tous rejetés "
                  f"(doublons) : augmenter --inner-colours ou --max-attempts",
                  file=sys.stderr)
            return 1
        # Graine textuelle : random.Random la passe par sha512, donc
        # reproductible d'une exécution à l'autre (PYTHONHASHSEED ne randomise
        # que le hash des str/bytes, jamais cette dérivation-là).
        rng = random.Random(f"etii-clone:{args.seed}:{attempt}")
        try:
            _edges, board, pieces = draw_instance(n, k, m, rng)
            break
        except DrawRejected:
            continue

    hints = build_hints(n, board, args.hints)

    prefix = args.prefix if args.prefix is not None else f"{n}_{k}_{args.seed}"
    os.makedirs(args.out_dir, exist_ok=True)
    pieces_path = os.path.join(args.out_dir, f"pieces_{prefix}.csv")
    indices_path = os.path.join(args.out_dir, f"indices_{prefix}.csv")
    solution_path = os.path.join(args.out_dir, f"solution_{prefix}.txt")

    write_pieces(pieces_path, pieces)
    write_indices(indices_path, hints)
    write_solution(solution_path, n, k, m, args.seed, board)

    errors = check_solution(pieces_path, solution_path, indices_path)
    if errors:
        print(f"AUTO-CONTRÔLE ÉCHOUÉ ({len(errors)} erreurs) :", file=sys.stderr)
        for e in errors[:20]:
            print(f"  - {e}", file=sys.stderr)
        return 1

    rc = 0 if args.no_validate else run_validate_pieces(pieces_path)

    half_edges = Counter()
    for _pid, top, right, bottom, left in pieces:
        half_edges.update(v for v in (top, right, bottom, left) if v != 0)
    inner_counts = sorted({half_edges[c] for c in range(1, k + 1)})
    frame_counts = sorted({half_edges[c] for c in range(k + 1, k + m + 1)})

    print(f"clone {n}×{n} : {n * n} pièces, {k} couleurs intérieures, "
          f"{m} de cadre, graine {args.seed} (tirage retenu : {attempt})")
    print(f"  demi-arêtes par couleur : intérieures {inner_counts}, cadre {frame_counts}")
    print(f"  indices : {len(hints)}")
    print(f"  {pieces_path}")
    print(f"  {indices_path}")
    print(f"  {solution_path}")
    print(f"  auto-contrôle : solution ré-assemblée depuis les fichiers, "
          f"toutes les arêtes concordent")
    print(f"\n  make WERROR=1 CPPFLAGS=-DETERN_PARTS={n * n}")
    print(f"  ./eternityII test {pieces_path}"
          + (f" --indices-file {indices_path}" if hints else "")
          + " --stop-on-solution")
    return rc


if __name__ == "__main__":
    sys.exit(main())
