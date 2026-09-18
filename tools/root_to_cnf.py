#!/usr/bin/env python3
"""
root_to_cnf.py — encode une RACINE d'Eternity II en DIMACS CNF (oracle CDCL).

Pourquoi cet outil existe
=========================
Le dépôt sait mesurer le coût de RÉFUTATION d'une racine avec son propre moteur
DFS (`make bench-refutation`, docs/tests_et_ci.md). Il ne sait pas dire si ce
coût est intrinsèque à l'instance ou propre à la famille d'inférence du moteur.
`docs/conception/elagage_recherche.md` §4.11 laisse explicitement non mesuré le
résidu « propagation des ensembles de conflit » — c'est-à-dire tout ce qu'un
moteur à APPRENTISSAGE DE CLAUSES (CDCL) sait faire et que le DFS ne sait pas.

Cet outil produit l'instance SAT équivalente à « ce sous-arbre contient-il une
solution ? », pour la soumettre à kissat/CaDiCaL. Il ne touche à aucun fichier
de `src/` : c'est un instrument de mesure, pas un moteur.
Campagne et verdict : `docs/conception/oracle_cdcl.md`.

Conventions du dépôt respectées ici (VÉRIFIÉES, pas devinées)
=============================================================
- `data/pieces.csv` : `ntiles: N` puis `id top left bottom right` — c'est
  l'ordre que lit `read_parts` (src/core/readdata.c), et le seul qui compte.
  (`tools/validate_pieces.py` documente un autre ordre ; cf. l'en-tête de
  `tools/gen_clone.py`, l'écart n'est qu'un miroir du plateau.)
- `grid[x][y]` : `x` est la COLONNE, `y` la LIGNE ; `top` regarde `y-1`,
  `bottom` regarde `y+1`, `left` regarde `x-1`, `right` regarde `x+1`
  (src/core/possibility.c, `what_search_in_grid_to_key`).
- Case vide = `-2`. Sinon la case contient `id + ETERN_PARTS * rotation`
  (`id_for_rotated_part`, src/core/part.c) : `id = ((v-1) % N) + 1` et
  `rotation = (v-1) // N`.
- Rotation : `rotatePart` (src/core/part.c) applique `top <- left`,
  `right <- top`, `bottom <- right`, `left <- bottom` par quart de tour, donc
  pour la rotation `r` la face `i` vaut `orig[(i - r) % 4]` avec
  `orig = (top, right, bottom, left)`.
- Couleur 0 = gris, la couleur du CADRE.

Le décodage a été contrôlé sur les 32 480 paquets de `eternityII.back` :
0 violation d'adjacence, 0 violation de cadre, `b_faceused` en accord avec la
grille sur tous les paquets, et les 5 indices de `data/indices.csv` présents et
corrects partout. Voir `--check-back`.

L'encodage (compact, d'après Heule 2008)
========================================
M. Heule, « Solving Edge-Matching Problems with Satisfiability Solvers »
(SAT 2008). L'idée qui rend l'encodage compact : ne JAMAIS comparer deux pièces
voisines deux à deux (quadratique en nombre de pièces et par arête), mais faire
transiter l'appariement par une VARIABLE DE COULEUR portée par l'arête. Chaque
placement implique la couleur qu'il présente sur chacune de ses arêtes ;
« exactement une couleur par arête » fait le reste.

Soit une racine à `A` pièces posées sur un plateau `S x S` (`N = S*S`) :
`E = N - A` cases vides et `F = N - A` pièces libres (les effectifs sont égaux
par construction).

Variables
---------
1. `x[c][p][r]` — « la pièce `p`, tournée de `r`, occupe la case vide `c` ».
   Engendrées seulement pour les triplets LOCALEMENT compatibles (voir le
   filtre ci-dessous). Les rotations qui donnent le même quadruplet de faces
   sont dédoublonnées (une pièce à symétrie de rotation n'a pas 4 rotations
   distinctes ; `data/pieces.csv` n'en contient aucune, mais un clone pourrait).
2. `e[arête][couleur]` — « l'arête porte cette couleur ». Engendrées seulement
   pour les arêtes LIBRES, c'est-à-dire celles dont les DEUX cases sont vides.
   Une arête vers l'extérieur du plateau est fixée à 0, une arête vers une case
   déjà posée est fixée à la couleur de cette pièce : dans les deux cas la
   contrainte est absorbée par le filtre de candidats, aucune variable.
3. Variables auxiliaires de l'encodage « au plus un » séquentiel (Sinz).

Filtre de candidats (une seule passe, aucun point fixe)
-------------------------------------------------------
`(p, r)` est candidat en `c` si, pour chaque côté :
  - côté hors plateau : la face vaut 0 ;
  - côté vers une case posée : la face vaut la face en regard du voisin ;
  - côté vers une case vide : la face est non nulle (voir ci-dessous).
C'est exactement le test que fait le forward-check du moteur C. Une seule
passe, volontairement : l'inférence est le travail du solveur, pas celui de
l'encodeur.

« Pas de gris sur une arête intérieure » n'est pas une hypothèse gratuite, et
n'est appliqué que si la structure du jeu de pièces le PROUVE (contrôle
`structural_grey_check`) : s'il y a exactement 4 pièces à deux faces grises,
`4*(S-2)` à une seule et zéro face grise ailleurs, alors le nombre total de
faces grises (`4*2 + 4*(S-2)` = `4*S`) égale exactement le nombre de côtés
tournés vers l'extérieur du plateau. Dans toute grille COMPLÈTE, chaque face
grise est donc consommée par le cadre, et une adjacence gris/gris intérieure
est impossible. Si le contrôle échoue, le filtre est désactivé et la couleur 0
entre dans le domaine des arêtes intérieures — le moteur C, lui, autoriserait
cette adjacence sur un plateau PARTIEL, mais la question posée porte sur les
COMPLÉTIONS, où elle ne peut pas survivre.

Clauses
-------
- Exactement une pièce par case vide : `ALO` (une clause) + `AMO`.
- Exactement une case par pièce libre : `ALO` + `AMO`.
- Exactement une couleur par arête libre : `ALO` + `AMO` (domaines ≤ ~23
  couleurs, `AMO` par paires).
- Implication placement → couleur : pour chaque candidat et chaque côté libre,
  `(¬x[c][p][r] ∨ e[arête][couleur présentée])`.
- `AMO` : par paires jusqu'à `--amo-pairwise-max` littéraux (défaut 20), sinon
  encodage séquentiel de Sinz (`k-1` auxiliaires, `3k-4` clauses) — les groupes
  « une pièce par case » comptent plusieurs centaines de littéraux, l'encodage
  par paires y produirait des dizaines de millions de clauses.

Le domaine de couleurs d'une arête libre est l'INTERSECTION des couleurs que
ses deux extrémités savent présenter ; un candidat qui présenterait une couleur
hors de ce domaine est retiré. Un domaine vide, ou une case sans candidat, rend
l'instance trivialement insatisfiable : la clause vide est émise et le statut
`UNSAT_AT_ENCODING` est rapporté (c'est la condition même du forward-check du
moteur C — le DFS la voit aussi, et pour zéro nœud).

Taille produite (racine réelle de 139 pièces, `eternityII.back`)
---------------------------------------------------------------
Voir `docs/conception/oracle_cdcl.md` §3 pour le tableau complet ; l'ordre de
grandeur est d'environ 1,6·10^5 variables et 7·10^5 clauses.

Contrôle positif (`--self-test`)
================================
Un UNSAT ne veut rien dire tant que l'encodage n'a pas trouvé un SAT là où une
solution existe. `--self-test` enchaîne :
  1. le puzzle 16 pièces du dépôt (`data/pieces16.csv`, plateau vide) ;
  2. un clone 16x16 à solution connue produit par `tools/gen_clone.py`, amputé
     de ses dernières pièces (`--solution` / `--keep`) — même chemin de code
     qu'une racine réelle, à pleine taille ;
et, dans les deux cas, RE-VÉRIFIE le modèle rendu par le solveur pièce par
pièce : une et une seule pièce par case, une et une seule case par pièce, et
les quatre faces de chaque case contrôlées contre leurs voisines et le cadre.

Exemples
========
    # encoder la 1re racine de 139 pièces du stock
    python3 tools/root_to_cnf.py --back eternityII.back --placed 139 --rank 0 \\
        --out /tmp/r.cnf --map /tmp/r.map.json

    # résoudre, puis re-vérifier un éventuel modèle
    kissat /tmp/r.cnf > /tmp/r.log ; \\
    python3 tools/root_to_cnf.py --verify --map /tmp/r.map.json --model /tmp/r.log

    # contrôle positif complet (exige un solveur dans le PATH ou --solver)
    python3 tools/root_to_cnf.py --self-test --solver .../kissat
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

# Côtés, dans l'ordre du dépôt : top, right, bottom, left.
TOP, RIGHT, BOTTOM, LEFT = 0, 1, 2, 3
# Déplacement (dx, dy) du voisin, pour chaque côté.
DELTA = {TOP: (0, -1), RIGHT: (1, 0), BOTTOM: (0, 1), LEFT: (-1, 0)}
GREY = 0


# --------------------------------------------------------------------------
# Lecture des pièces et rotations
# --------------------------------------------------------------------------

def read_pieces(path):
    """`ntiles: N` puis `id top left bottom right` (ordre de read_parts).

    @return (dict id -> (top, right, bottom, left), N)
    """
    with open(path) as fh:
        text = fh.read()
    lines = [ln.strip() for ln in text.split("\n")]
    header = lines[0]
    if not header.startswith("ntiles:"):
        raise ValueError(f"{path} : première ligne `ntiles: N` attendue")
    ntiles = int(header.split(":")[1])
    pieces = {}
    for ln in lines[1:]:
        if not ln:
            continue
        pid, top, left, bottom, right = (int(t) for t in ln.split())
        pieces[pid] = (top, right, bottom, left)
    if len(pieces) != ntiles:
        raise ValueError(f"{path} : {len(pieces)} pièces lues, {ntiles} annoncées")
    return pieces, ntiles


def faces(orig, rot):
    """Faces de la pièce `orig` tournée de `rot` quarts de tour.

    `rotatePart` fait `top <- left, right <- top, bottom <- right, left <- bottom`
    par quart de tour, d'où `face[i] = orig[(i - rot) % 4]`.
    """
    return tuple(orig[(i - rot) % 4] for i in range(4))


def distinct_rotations(orig):
    """Rotations donnant des quadruplets de faces deux à deux distincts."""
    seen = {}
    for r in range(4):
        f = faces(orig, r)
        if f not in seen:
            seen[f] = r
    return sorted(seen.values())


def structural_grey_check(pieces, size):
    """Le gris ne peut-il apparaître qu'en bordure ? (cf. l'en-tête)

    @return (bool, message)
    """
    by_zeros = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0}
    for orig in pieces.values():
        by_zeros[sum(1 for f in orig if f == GREY)] += 1
    want_corner, want_edge = 4, 4 * (size - 2)
    ok = (by_zeros[2] == want_corner and by_zeros[1] == want_edge
          and by_zeros[3] == 0 and by_zeros[4] == 0)
    msg = (f"pièces à 0/1/2/3/4 faces grises : {by_zeros[0]}/{by_zeros[1]}/"
           f"{by_zeros[2]}/{by_zeros[3]}/{by_zeros[4]} "
           f"(attendu coins={want_corner}, bords={want_edge})")
    return ok, msg


# --------------------------------------------------------------------------
# Lecture d'une racine
# --------------------------------------------------------------------------

def packet_size(size, ntiles):
    """Taille d'un `struct possibility_packet` (src/core/possibility.h).

    Champs : x, y (uint8), grid[S][S] (int16), alloc (uint16),
    min_candidats (int16), b_faceused[FACES_USED_SIZE] (uint16, aligné 16),
    checked (uint8) ; le tout `__packed__` mais avec `aligned(16)` sur
    `b_faceused`, donc bourrage avant lui et taille arrondie à 16.
    """
    off = 2 + 2 * size * size + 2 + 2
    off = (off + 15) // 16 * 16          # alignement de b_faceused
    faces_used = (ntiles // 16) + 1
    off += 2 * faces_used + 1            # b_faceused + checked
    return (off + 15) // 16 * 16


def decode_packet(blob, size, ntiles):
    """@return dict (x, y) -> (id, rotation) des cases occupées."""
    grid = struct.unpack_from("<%dh" % (size * size), blob, 2)
    placed = {}
    for x in range(size):
        for y in range(size):
            v = grid[x * size + y]
            if v == -2:
                continue
            placed[(x, y)] = (((v - 1) % ntiles) + 1, (v - 1) // ntiles)
    return placed


def iter_back(path, size, ntiles):
    """Itère `(index, placed_count, board)` sur un fichier `.back`."""
    psz = packet_size(size, ntiles)
    total = os.path.getsize(path)
    if total % psz:
        raise ValueError(f"{path} : {total} octets, non multiple de {psz}")
    with open(path, "rb") as fh:
        index = 0
        while True:
            blob = fh.read(psz)
            if len(blob) < psz:
                return
            board = decode_packet(blob, size, ntiles)
            yield index, len(board), board
            index += 1


def select_root(path, size, ntiles, index=None, placed=None, rank=0,
                min_placed=None, max_placed=None):
    """Sélectionne une racine par index absolu, ou par (filtre de taille, rang).

    Le mode `(filtre, rang)` REPRODUIT à l'identique la sélection de
    `tests/bench/bench_refutation.c --min-pieces A --max-pieces B` : les paquets
    sont lus dans l'ordre du fichier, ceux dont le nombre de pièces posées tombe
    dans l'intervalle sont retenus, et les `--max-roots` premiers sont joués. Le
    rang `k` correspond donc exactement à la ligne `back#k` du banc — c'est ce
    qui APPARIE une mesure CDCL à sa mesure DFS.
    """
    lo = min_placed if min_placed is not None else placed
    hi = max_placed if max_placed is not None else placed
    seen = 0
    for idx, count, board in iter_back(path, size, ntiles):
        if index is not None:
            if idx == index:
                return idx, board
            continue
        if lo is not None and count < lo:
            continue
        if hi is not None and count > hi:
            continue
        if seen == rank:
            return idx, board
        seen += 1
    raise ValueError("racine introuvable dans le stock")


def read_solution_file(path):
    """Relit un `solution_*.txt` de `tools/gen_clone.py`. @return (n, board)."""
    n = None
    cells = {}
    with open(path) as fh:
        for raw in fh:
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
    return n, cells


# --------------------------------------------------------------------------
# Encodage
# --------------------------------------------------------------------------

class Cnf:
    def __init__(self):
        self.nvars = 0
        self.clauses = []
        self.empty = False

    def new_var(self):
        self.nvars += 1
        return self.nvars

    def add(self, clause):
        if not clause:
            self.empty = True
        self.clauses.append(clause)

    def amo(self, lits, pairwise_max):
        """Au plus un littéral vrai."""
        k = len(lits)
        if k <= 1:
            return
        if k <= pairwise_max:
            for i in range(k):
                for j in range(i + 1, k):
                    self.add([-lits[i], -lits[j]])
            return
        # Encodage séquentiel de Sinz (k-1 auxiliaires).
        s = [self.new_var() for _ in range(k - 1)]
        self.add([-lits[0], s[0]])
        self.add([-lits[k - 1], -s[k - 2]])
        for i in range(1, k - 1):
            self.add([-lits[i], s[i]])
            self.add([-s[i - 1], s[i]])
            self.add([-lits[i], -s[i - 1]])


def encode(board, pieces, size, amo_pairwise_max=20, forbid_inner_grey=None):
    """Encode « ce plateau partiel admet-il une complétion ? » en CNF.

    @return dict décrivant le CNF, la table des variables et un bilan.
    """
    ntiles = size * size
    cells = [(x, y) for x in range(size) for y in range(size)]
    empties = [c for c in cells if c not in board]
    used = {pid for pid, _ in board.values()}
    free = sorted(p for p in pieces if p not in used)
    if len(free) != len(empties):
        raise ValueError(f"{len(empties)} cases vides mais {len(free)} pièces libres")

    grey_ok, grey_msg = structural_grey_check(pieces, size)
    if forbid_inner_grey is None:
        forbid_inner_grey = grey_ok

    rots = {p: distinct_rotations(pieces[p]) for p in free}

    # Contrainte fixe de chaque côté de chaque case vide : couleur imposée, ou
    # None si l'arête est libre (les deux cases sont vides).
    fixed = {}
    for (x, y) in empties:
        per_side = {}
        for side, (dx, dy) in DELTA.items():
            nx, ny = x + dx, y + dy
            if not (0 <= nx < size and 0 <= ny < size):
                per_side[side] = GREY
            elif (nx, ny) in board:
                npid, nrot = board[(nx, ny)]
                per_side[side] = faces(pieces[npid], nrot)[(side + 2) % 4]
            else:
                per_side[side] = None
        fixed[(x, y)] = per_side

    # Candidats (p, r) par case vide.
    cand = {}
    for c in empties:
        per_side = fixed[c]
        lst = []
        for p in free:
            for r in rots[p]:
                f = faces(pieces[p], r)
                ok = True
                for side in range(4):
                    want = per_side[side]
                    if want is None:
                        if forbid_inner_grey and f[side] == GREY:
                            ok = False
                            break
                    elif f[side] != want:
                        ok = False
                        break
                if ok:
                    lst.append((p, r))
        cand[c] = lst

    # Arêtes libres : couple de cases vides adjacentes. Identifiées par
    # (x, y, side) avec side ∈ {RIGHT, BOTTOM} pour n'en garder qu'un exemplaire.
    edges = {}
    for (x, y) in empties:
        for side in (RIGHT, BOTTOM):
            dx, dy = DELTA[side]
            nb = (x + dx, y + dy)
            if 0 <= nb[0] < size and 0 <= nb[1] < size and nb not in board:
                edges[((x, y), side)] = nb

    def edge_of(cell, side):
        """Clé canonique de l'arête entre `cell` et son voisin du côté `side`."""
        if side in (RIGHT, BOTTOM):
            return (cell, side)
        dx, dy = DELTA[side]
        nb = (cell[0] + dx, cell[1] + dy)
        return (nb, RIGHT if side == LEFT else BOTTOM)

    # Domaine de couleurs d'une arête : intersection de ce que ses deux
    # extrémités savent présenter.
    domain = {}
    for key, nb in edges.items():
        cell, side = key
        mine = {faces(pieces[p], r)[side] for (p, r) in cand[cell]}
        opp = (side + 2) % 4
        theirs = {faces(pieces[p], r)[opp] for (p, r) in cand[nb]}
        domain[key] = sorted(mine & theirs)

    # Un candidat présentant une couleur hors domaine est retiré (une passe).
    for c in empties:
        keep = []
        for (p, r) in cand[c]:
            f = faces(pieces[p], r)
            ok = True
            for side in range(4):
                if fixed[c][side] is None:
                    if f[side] not in domain[edge_of(c, side)]:
                        ok = False
                        break
            if ok:
                keep.append((p, r))
        cand[c] = keep

    cnf = Cnf()
    xvar = {}
    for c in empties:
        for (p, r) in cand[c]:
            xvar[(c, p, r)] = cnf.new_var()
    evar = {}
    for key, cols in domain.items():
        for col in cols:
            evar[(key, col)] = cnf.new_var()

    stats = {
        "placement_vars": len(xvar),
        "colour_vars": len(evar),
        "free_edges": len(edges),
        "empty_cells": len(empties),
        "free_pieces": len(free),
        "forbid_inner_grey": bool(forbid_inner_grey),
        "grey_structure": grey_msg,
        "grey_structure_ok": grey_ok,
        "min_candidates_per_cell": min((len(cand[c]) for c in empties), default=0),
        "max_candidates_per_cell": max((len(cand[c]) for c in empties), default=0),
        "unsat_at_encoding": False,
        "unsat_reason": None,
    }

    # Case sans candidat / arête sans couleur : insatisfiable sans chercher.
    for c in empties:
        if not cand[c]:
            cnf.add([])
            stats["unsat_at_encoding"] = True
            stats["unsat_reason"] = f"case {c} sans candidat"
    for key, cols in domain.items():
        if not cols:
            cnf.add([])
            stats["unsat_at_encoding"] = True
            stats["unsat_reason"] = stats["unsat_reason"] or f"arête {key} sans couleur"

    # Exactement une pièce par case vide.
    for c in empties:
        lits = [xvar[(c, p, r)] for (p, r) in cand[c]]
        cnf.add(list(lits))
        cnf.amo(lits, amo_pairwise_max)

    # Exactement une case par pièce libre.
    by_piece = {p: [] for p in free}
    for (c, p, r), v in xvar.items():
        by_piece[p].append(v)
    for p in free:
        lits = by_piece[p]
        if not lits:
            cnf.add([])
            stats["unsat_at_encoding"] = True
            stats["unsat_reason"] = stats["unsat_reason"] or f"pièce {p} sans case"
            continue
        cnf.add(list(lits))
        cnf.amo(lits, amo_pairwise_max)

    # Exactement une couleur par arête libre.
    for key, cols in domain.items():
        lits = [evar[(key, col)] for col in cols]
        if lits:
            cnf.add(list(lits))
            cnf.amo(lits, amo_pairwise_max)

    # Placement -> couleur présentée sur chaque côté libre.
    for c in empties:
        for (p, r) in cand[c]:
            f = faces(pieces[p], r)
            v = xvar[(c, p, r)]
            for side in range(4):
                if fixed[c][side] is None:
                    key = edge_of(c, side)
                    cnf.add([-v, evar[(key, f[side])]])

    stats["nvars"] = cnf.nvars
    stats["nclauses"] = len(cnf.clauses)
    return {
        "cnf": cnf,
        "xvar": xvar,
        "stats": stats,
        "board": board,
        "size": size,
        "free": free,
        "empties": empties,
    }


def write_dimacs(cnf, path):
    with open(path, "w") as fh:
        fh.write(f"p cnf {cnf.nvars} {len(cnf.clauses)}\n")
        out = []
        for cl in cnf.clauses:
            out.append(" ".join(str(l) for l in cl) + " 0\n")
            if len(out) >= 4096:
                fh.write("".join(out))
                out = []
        fh.write("".join(out))


def write_map(enc, path, extra=None):
    data = {
        "size": enc["size"],
        "board": [[x, y, p, r] for (x, y), (p, r) in sorted(enc["board"].items())],
        "placement": [[v, c[0], c[1], p, r] for (c, p, r), v in enc["xvar"].items()],
        "stats": enc["stats"],
    }
    if extra:
        data.update(extra)
    with open(path, "w") as fh:
        json.dump(data, fh)


# --------------------------------------------------------------------------
# Vérification d'un modèle
# --------------------------------------------------------------------------

def parse_model(path):
    """Lit les lignes `v ...` d'une sortie DIMACS. @return set des littéraux vrais."""
    true_lits = set()
    with open(path) as fh:
        for line in fh:
            if not line.startswith("v "):
                continue
            for tok in line[2:].split():
                val = int(tok)
                if val > 0:
                    true_lits.add(val)
    return true_lits


def verify_model(mapping_path, model_path, pieces_path):
    """Re-vérifie un modèle pièce par pièce. @return (ok, liste de messages)."""
    with open(mapping_path) as fh:
        data = json.load(fh)
    pieces, _ = read_pieces(pieces_path)
    size = data["size"]
    board = {(x, y): (p, r) for x, y, p, r in data["board"]}
    true_lits = parse_model(model_path)
    errors = []

    assigned = {}
    for v, x, y, p, r in data["placement"]:
        if v in true_lits:
            if (x, y) in assigned:
                errors.append(f"case ({x},{y}) reçoit deux placements")
            assigned[(x, y)] = (p, r)

    full = dict(board)
    for c, v in assigned.items():
        if c in full:
            errors.append(f"case {c} déjà occupée par la racine")
        full[c] = v

    if len(full) != size * size:
        errors.append(f"{len(full)} cases remplies sur {size * size}")

    seen = {}
    for c, (p, r) in full.items():
        if p in seen:
            errors.append(f"pièce {p} placée en {seen[p]} ET en {c}")
        seen[p] = c
        if p not in pieces:
            errors.append(f"pièce {p} inconnue")
            continue
        if r not in range(4):
            errors.append(f"rotation {r} invalide en {c}")

    # Contrôle couleur par couleur, cadre compris.
    for (x, y), (p, r) in full.items():
        if p not in pieces:
            continue
        f = faces(pieces[p], r)
        for side, (dx, dy) in DELTA.items():
            nx, ny = x + dx, y + dy
            if not (0 <= nx < size and 0 <= ny < size):
                if f[side] != GREY:
                    errors.append(f"({x},{y}) côté {side} : {f[side]} au bord, attendu 0")
                continue
            if (nx, ny) not in full:
                errors.append(f"voisin ({nx},{ny}) absent")
                continue
            np_, nr = full[(nx, ny)]
            if np_ not in pieces:
                continue
            nf = faces(pieces[np_], nr)[(side + 2) % 4]
            if f[side] != nf:
                errors.append(f"({x},{y}) côté {side} : {f[side]} != {nf} de ({nx},{ny})")
            if f[side] == GREY:
                errors.append(f"({x},{y})-({nx},{ny}) : adjacence grise intérieure")
    return (not errors), errors


# --------------------------------------------------------------------------
# Contrôle positif
# --------------------------------------------------------------------------

def find_solver(explicit=None):
    cands = []
    if explicit:
        cands.append(explicit)
    env = os.environ.get("KISSAT")
    if env:
        cands.append(env)
    cands += [
        "kissat",
        os.path.expanduser("~/etii_mesures_2026-09-18b/kissat/build/kissat"),
    ]
    for c in cands:
        if os.path.isabs(c) or "/" in c:
            if os.path.isfile(c) and os.access(c, os.X_OK):
                return c
        else:
            from shutil import which
            w = which(c)
            if w:
                return w
    return None


def run_solver(solver, cnf_path, log_path, timeout=None, extra_args=()):
    """Lance le solveur. @return (statut, texte)."""
    cmd = [solver, *extra_args, cnf_path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = proc.stdout
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
        with open(log_path, "w") as fh:
            fh.write(out)
        return "TIMEOUT", out
    with open(log_path, "w") as fh:
        fh.write(out)
    if "s SATISFIABLE" in out:
        return "SAT", out
    if "s UNSATISFIABLE" in out:
        return "UNSAT", out
    return "UNKNOWN", out


def self_test(solver=None, workdir=None, verbose=True):
    """Contrôle positif : SAT trouvé et modèle re-vérifié sur instances connues."""
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    solver = find_solver(solver)
    if solver is None:
        print("auto-test : aucun solveur trouvé (PATH, $KISSAT ou --solver)")
        return 2
    tmp = workdir or tempfile.mkdtemp(prefix="root_to_cnf_")
    os.makedirs(tmp, exist_ok=True)
    failures = 0

    def one(name, pieces_path, size, board):
        nonlocal failures
        pieces, _ = read_pieces(pieces_path)
        enc = encode(board, pieces, size)
        cnf_p = os.path.join(tmp, f"{name}.cnf")
        map_p = os.path.join(tmp, f"{name}.map.json")
        log_p = os.path.join(tmp, f"{name}.log")
        write_dimacs(enc["cnf"], cnf_p)
        write_map(enc, map_p)
        st = enc["stats"]
        status, _ = run_solver(solver, cnf_p, log_p, timeout=600)
        ok = status == "SAT"
        detail = ""
        if ok:
            good, errs = verify_model(map_p, log_p, pieces_path)
            ok = good
            if not good:
                detail = " ; " + " | ".join(errs[:5])
        if verbose:
            print(f"auto-test {name:<22} {st['nvars']:>8} var {st['nclauses']:>9} cl "
                  f"-> {status}{'' if ok else '  ÉCHEC' + detail}")
        if not ok:
            failures += 1
        return ok

    # 1. Puzzle 16 pièces du dépôt, plateau vide : une solution existe.
    p16 = os.path.join(repo, "data", "pieces16.csv")
    one("pieces16-vide", p16, 4, {})

    # 2. Clone 16x16 à solution connue, amputé de ses N dernières pièces :
    #    même chemin de code qu'une racine réelle, à pleine taille.
    clone_dir = os.path.join(tmp, "clone")
    os.makedirs(clone_dir, exist_ok=True)
    gen = os.path.join(repo, "tools", "gen_clone.py")
    sol = None
    cp = None
    try:
        subprocess.run(
            [sys.executable, gen, "--size", "16", "--inner-colours", "17",
             "--seed", "7", "--out-dir", clone_dir, "--prefix", "cdcl16",
             "--no-validate"],
            capture_output=True, text=True, check=True, cwd=repo)
        sol = os.path.join(clone_dir, "solution_cdcl16.txt")
        cp = os.path.join(clone_dir, "pieces_cdcl16.csv")
    except Exception as exc:                                    # pragma: no cover
        print(f"auto-test clone : génération impossible ({exc}) — étape ignorée")
    if sol and os.path.isfile(sol):
        n, cells = read_solution_file(sol)
        order = sorted(cells)
        for keep in (139, 120):
            board = {c: cells[c] for c in order[:keep]}
            one(f"clone16-{keep}pieces", cp, n, board)

    print(f"auto-test : {failures} échec(s)")
    return 1 if failures else 0


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Encode une racine d'Eternity II en DIMACS CNF (oracle CDCL).")
    ap.add_argument("--pieces", default="data/pieces.csv")
    ap.add_argument("--size", type=int, default=None,
                    help="côté du plateau (déduit de ntiles par défaut)")
    src = ap.add_argument_group("source de la racine")
    src.add_argument("--back", help="stock serveur (.back)")
    src.add_argument("--index", type=int, help="index absolu du paquet dans le .back")
    src.add_argument("--placed", type=int, help="nombre de pièces posées recherché")
    src.add_argument("--min-placed", type=int, help="borne basse du filtre de taille")
    src.add_argument("--max-placed", type=int, help="borne haute du filtre de taille")
    src.add_argument("--rank", type=int, default=0,
                     help="rang parmi les paquets retenus par le filtre (= back#N du banc)")
    src.add_argument("--solution", help="solution_*.txt de tools/gen_clone.py")
    src.add_argument("--keep", type=int, help="nombre de cases gardées de --solution")
    src.add_argument("--empty", action="store_true", help="plateau vide")
    ap.add_argument("--out", help="fichier CNF de sortie")
    ap.add_argument("--map", dest="mapfile", help="table des variables (JSON)")
    ap.add_argument("--stats-json", help="écrit le bilan d'encodage en JSON")
    ap.add_argument("--amo-pairwise-max", type=int, default=20)
    ap.add_argument("--allow-inner-grey", action="store_true",
                    help="n'interdit pas le gris sur les arêtes intérieures")
    ap.add_argument("--verify", action="store_true", help="re-vérifie un modèle")
    ap.add_argument("--model", help="sortie du solveur à vérifier")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--solver", help="binaire SAT pour --self-test")
    ap.add_argument("--check-back", help="contrôle intégral du décodage d'un .back")
    ap.add_argument("--list-back", help="liste index/pièces posées d'un .back")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test(args.solver)

    if args.verify:
        if not (args.mapfile and args.model):
            ap.error("--verify exige --map et --model")
        ok, errs = verify_model(args.mapfile, args.model, args.pieces)
        if ok:
            print("modèle VÉRIFIÉ : placement complet, couleurs et cadre corrects")
            return 0
        print(f"modèle INVALIDE : {len(errs)} erreur(s)")
        for e in errs[:20]:
            print("  " + e)
        return 1

    pieces, ntiles = read_pieces(args.pieces)
    size = args.size or int(round(ntiles ** 0.5))
    if size * size != ntiles:
        ap.error(f"{ntiles} pièces : plateau non carré, précisez --size")

    if args.check_back:
        return check_back(args.check_back, pieces, size, ntiles)

    if args.list_back:
        for idx, count, _ in iter_back(args.list_back, size, ntiles):
            print(idx, count)
        return 0

    if args.back:
        if args.index is None and args.placed is None and args.min_placed is None:
            ap.error("--back exige --index, --placed ou --min-placed/--max-placed")
        idx, board = select_root(args.back, size, ntiles,
                                 index=args.index, placed=args.placed, rank=args.rank,
                                 min_placed=args.min_placed, max_placed=args.max_placed)
        origin = {"back": os.path.abspath(args.back), "index": idx,
                  "placed": args.placed, "min_placed": args.min_placed,
                  "max_placed": args.max_placed, "rank": args.rank,
                  "placed_count": len(board)}
    elif args.solution:
        n, cells = read_solution_file(args.solution)
        size = n
        keep = args.keep if args.keep is not None else len(cells)
        order = sorted(cells)
        board = {c: cells[c] for c in order[:keep]}
        origin = {"solution": os.path.abspath(args.solution), "keep": keep}
    elif args.empty:
        board = {}
        origin = {"empty": True}
    else:
        ap.error("précisez --back, --solution, --empty, --verify ou --self-test")

    enc = encode(board, pieces, size,
                 amo_pairwise_max=args.amo_pairwise_max,
                 forbid_inner_grey=False if args.allow_inner_grey else None)
    st = enc["stats"]
    st["origin"] = origin
    st["pieces_file"] = os.path.abspath(args.pieces)

    if args.out:
        write_dimacs(enc["cnf"], args.out)
    if args.mapfile:
        write_map(enc, args.mapfile, extra={"pieces_file": st["pieces_file"],
                                            "origin": origin})
    if args.stats_json:
        with open(args.stats_json, "w") as fh:
            json.dump(st, fh)
    print(json.dumps({k: v for k, v in st.items() if k != "grey_structure"},
                     sort_keys=True))
    return 0


def check_back(path, pieces, size, ntiles):
    """Contrôle intégral du décodage : adjacences, cadre, doublons.

    Le stock est produit par le moteur C : si le décodage (ordre des colonnes du
    CSV, sens de rotation, orientation de `grid`) était faux, ces contrôles
    échoueraient massivement.
    """
    bad_adj = bad_border = bad_dup = 0
    total = 0
    for _, _, board in iter_back(path, size, ntiles):
        total += 1
        seen = set()
        for (x, y), (p, r) in board.items():
            if p in seen:
                bad_dup += 1
            seen.add(p)
            f = faces(pieces[p], r)
            for side, (dx, dy) in DELTA.items():
                nx, ny = x + dx, y + dy
                outside = not (0 <= nx < size and 0 <= ny < size)
                if outside != (f[side] == GREY):
                    bad_border += 1
                if outside or (nx, ny) not in board:
                    continue
                np_, nr = board[(nx, ny)]
                if faces(pieces[np_], nr)[(side + 2) % 4] != f[side]:
                    bad_adj += 1
    print(f"{total} paquets : {bad_adj} adjacence(s) fausse(s), "
          f"{bad_border} incohérence(s) de cadre, {bad_dup} doublon(s)")
    return 0 if (bad_adj == bad_border == bad_dup == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
