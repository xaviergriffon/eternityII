#!/usr/bin/env python3
"""
Banc « oracle CDCL » : le DFS du dépôt contre un solveur SAT à APPRENTISSAGE.

La question
-----------
Le coût de réfutation d'une racine mesuré par `make bench-refutation` est-il
intrinsèque à l'instance, ou propre à la famille d'inférence du moteur ?
`docs/conception/elagage_recherche.md` §4.11 laisse explicitement non mesurée
« la propagation des ensembles de conflit » — tout ce qu'un moteur CDCL sait
faire et que le DFS ne sait pas. Ce banc soumet LES MÊMES racines aux deux.

Ce n'est pas un moteur : rien ici n'entre dans `src/`. C'est un oracle, au sens
« plafond de ce qu'une autre famille obtiendrait ».

Appariement
-----------
Une racine est désignée par (filtre de taille, rang). C'est la règle de
sélection de `tests/bench/bench_refutation.c` : les paquets du `.back` sont lus
DANS L'ORDRE DU FICHIER, ceux qui passent `--min-pieces`/`--max-pieces` sont
retenus, et les `--max-roots` premiers sont joués, étiquetés `back#0`, `back#1`…
Le rang `k` de ce banc est donc exactement la ligne `back#k` du banc de
réfutation, et `tools/root_to_cnf.py --min-placed/--max-placed/--rank` reproduit
la même sélection. La comparaison est appariée racine par racine.

Contrôle positif
----------------
Le banc REFUSE de mesurer tant que `tools/root_to_cnf.py --self-test` n'a pas
trouvé SAT, et re-vérifié le modèle pièce par pièce, sur des instances à
solution connue (le puzzle 16 pièces du dépôt et un clone 16x16 de
`tools/gen_clone.py`). Sans ce contrôle, un UNSAT ne prouve rien : un encodage
sur-contraint produit des UNSAT gratuits.

Un SAT sur une racine réelle serait soit une SOLUTION du puzzle, soit un bug
d'encodage : le banc s'arrête et sauve le modèle dans les deux cas.

Emploi
------
    make bench-cdcl
    make bench-cdcl BENCH_CDCL_ARGS="--min-pieces 100 --max-pieces 120 --roots 30"
    KISSAT=/chemin/vers/kissat make bench-cdcl

Le solveur n'est PAS fourni par le dépôt (aucun binaire téléchargé n'entre
ici) : il se cherche via `$KISSAT`, puis dans le `PATH`. Sources officielles :
github.com/arminbiere/kissat et github.com/arminbiere/cadical.
Campagne complète et verdict : `docs/conception/oracle_cdcl.md`.
"""

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ENC = os.path.join(REPO, "tools", "root_to_cnf.py")
REFUT = os.path.join(HERE, "bench_refutation")

ROW = re.compile(r"^back#(\d+)\s+(\d+)\s+\|\s+(\S+)\s+(\d+)\s+([0-9.]+) s\s+(\d+)")
CONFLICTS = re.compile(r"^c conflicts:\s+(\d+)")
PROCTIME = re.compile(r"^c process-time:.*?([0-9.]+)\s+seconds")


def find_solver():
    cand = os.environ.get("KISSAT") or shutil.which("kissat")
    if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
        return cand
    return None


def run_dfs(back, lo, hi, roots, budget):
    """Joue le banc de réfutation et rend une ligne par racine."""
    cmd = [REFUT, "--from-back", back, "--min-pieces", str(lo),
           "--max-pieces", str(hi), "--max-roots", str(roots),
           "--budget", str(budget), "--engines", "mrv"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO).stdout
    rows = []
    for line in out.split("\n"):
        m = ROW.match(line.strip())
        if m:
            rows.append({"rank": int(m.group(1)), "pieces": int(m.group(2)),
                         "status": m.group(3), "nodes": int(m.group(4)),
                         "seconds": float(m.group(5)), "fall": int(m.group(6))})
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--back", default=os.path.join(REPO, "eternityII.back"))
    ap.add_argument("--pieces", default=os.path.join(REPO, "data", "pieces.csv"))
    ap.add_argument("--min-pieces", type=int, default=130)
    ap.add_argument("--max-pieces", type=int, default=256)
    ap.add_argument("--roots", type=int, default=20)
    ap.add_argument("--budget", type=int, default=2000000)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--keep-cnf", action="store_true")
    ap.add_argument("--skip-self-test", action="store_true",
                    help="À N'EMPLOYER QUE si le contrôle positif vient d'être joué")
    args = ap.parse_args(argv)

    solver = find_solver()
    if solver is None:
        print("bench-cdcl : aucun solveur SAT trouvé.\n"
              "  Indiquez-en un : KISSAT=/chemin/vers/kissat make bench-cdcl\n"
              "  Source officielle : github.com/arminbiere/kissat "
              "(./configure && make, aucune installation système).", file=sys.stderr)
        return 2
    if not os.path.isfile(REFUT):
        print(f"bench-cdcl : {REFUT} absent — `make bench-refutation` d'abord,\n"
              "  ou employez la cible make qui le construit.", file=sys.stderr)
        return 2

    print(f"solveur : {solver}")
    if not args.skip_self_test:
        print("\n--- contrôle positif (obligatoire) ---")
        rc = subprocess.run([sys.executable, ENC, "--self-test", "--solver", solver],
                            cwd=REPO).returncode
        if rc != 0:
            print("bench-cdcl : contrôle positif en ÉCHEC, mesure abandonnée.",
                  file=sys.stderr)
            return 1

    print(f"\n--- DFS du dépôt (bench_refutation, plafond {args.budget} nœuds) ---")
    dfs = run_dfs(args.back, args.min_pieces, args.max_pieces, args.roots, args.budget)
    if not dfs:
        print("bench-cdcl : aucune racine retenue par le filtre.", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="bench_cdcl_")
    print(f"\n--- CDCL ({os.path.basename(solver)}, plafond {args.timeout} s) ---")
    print(f"{'racine':<10}{'pièces':>7}{'nœuds DFS':>12}{'temps DFS':>11}"
          f"{'conflits':>11}{'temps SAT':>11}{'encodage':>10}  statut")
    rows = []
    alert = 0
    for d in dfs:
        tag = f"back{d['rank']:04d}"
        cnf = os.path.join(tmp, tag + ".cnf")
        mp = os.path.join(tmp, tag + ".map.json")
        log = os.path.join(tmp, tag + ".log")
        cmd = [sys.executable, ENC, "--pieces", args.pieces, "--back", args.back,
               "--min-placed", str(args.min_pieces), "--max-placed", str(args.max_pieces),
               "--rank", str(d["rank"]), "--out", cnf, "--map", mp]
        t0 = time.time()
        subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, check=True)
        enc_s = time.time() - t0
        try:
            out = subprocess.run([solver, cnf], capture_output=True, text=True,
                                 timeout=args.timeout).stdout
            status = ("SAT" if "s SATISFIABLE" in out else
                      "UNSAT" if "s UNSATISFIABLE" in out else "?")
        except subprocess.TimeoutExpired:
            out, status = "", "TIMEOUT"
        with open(log, "w") as fh:
            fh.write(out)
        confl = next((int(m.group(1)) for m in
                      (CONFLICTS.match(l) for l in out.split("\n")) if m), None)
        secs = next((float(m.group(1)) for m in
                     (PROCTIME.match(l) for l in out.split("\n")) if m), None)
        if status == "SAT":
            alert += 1
            print(f"\n*** {tag} : le solveur rend SAT. Soit une SOLUTION du puzzle,\n"
                  f"*** soit un bug d'encodage. Modèle conservé : {log}\n")
            subprocess.run([sys.executable, ENC, "--verify", "--map", mp,
                            "--model", log, "--pieces", args.pieces], cwd=REPO)
        elif not args.keep_cnf:
            os.remove(cnf)
        print(f"{tag:<10}{d['pieces']:>7}{d['nodes']:>12}{d['seconds']:>10.3f}s"
              f"{(confl if confl is not None else -1):>11}"
              f"{(secs if secs is not None else -1):>10.2f}s{enc_s:>9.2f}s"
              f"  {d['status']}/{status}")
        rows.append((d, confl, secs, status, enc_s))

    print("\n--- bilan apparié ---")
    both = [(d, c, s) for d, c, s, st, _ in rows
            if d["status"] == "FERMÉ" and st == "UNSAT" and c is not None]
    print(f"racines : {len(rows)} ; fermées par le DFS : "
          f"{sum(1 for d, *_ in rows if d['status'] == 'FERMÉ')} ; "
          f"UNSAT par le solveur : {sum(1 for *_, st, _ in rows if st == 'UNSAT')}")
    dfs_open_sat_closed = [d for d, c, s, st, _ in rows
                           if d["status"] != "FERMÉ" and st in ("UNSAT", "SAT")]
    print(f"racines laissées OUVERTES par le DFS et tranchées par le solveur : "
          f"{len(dfs_open_sat_closed)}")
    if both:
        ratio_n = [d["nodes"] / max(c, 1) for d, c, _ in both]
        ratio_t = [d["seconds"] / max(s, 1e-6) for d, _, s in both]
        print(f"nœuds DFS / conflits CDCL — médiane {statistics.median(ratio_n):.1f}, "
              f"min {min(ratio_n):.1f}, max {max(ratio_n):.1f}")
        print(f"temps DFS / temps CDCL    — médiane {statistics.median(ratio_t):.4f}, "
              f"min {min(ratio_t):.4f}, max {max(ratio_t):.4f}")
        print(f"racines où le CDCL est au moins 10x plus rapide : "
              f"{sum(1 for r in ratio_t if r >= 10)}")
        print(f"racines où les conflits CDCL sont au moins 10x sous les nœuds DFS : "
              f"{sum(1 for r in ratio_n if r >= 10)}")
    print(f"\ntemps d'encodage total (Python, hors solveur) : "
          f"{sum(r[4] for r in rows):.1f} s")
    if not args.keep_cnf:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"CNF conservés dans {tmp}")
    return 1 if alert else 0


if __name__ == "__main__":
    sys.exit(main())
