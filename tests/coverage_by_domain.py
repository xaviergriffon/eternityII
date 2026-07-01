#!/usr/bin/env python3
"""Enrichit le rapport gcovr Markdown : note « écart Codecov » + section par domaine.

Lit le résumé JSON de gcovr (`--json-summary`, une entrée par fichier), agrège
les lignes / fonctions / branches par domaine (la 2e composante du chemin, p. ex.
`src/core/...` -> `core`), puis insère un tableau Markdown dans `coverage.md`
juste avant la section « ## 📄 File coverage » produite par gcovr.

Si le Cobertura XML est fourni (3e argument), insère EN PLUS, sous le tableau
« Overall coverage », une note expliquant pourquoi le pourcentage Codecov est
plus bas : gcovr compte une ligne comme couverte dès qu'elle est exécutée au
moins une fois (branches partielles incluses), tandis que Codecov range les
lignes de branchement partiellement couvertes dans un bucket « partial » qu'il
ne compte pas dans « covered ». La note affiche le nombre réel de lignes
partielles et l'équivalent « hits seuls » (le chiffre de l'en-tête Codecov).

Usage : coverage_by_domain.py <summary.json> <coverage.md> [cobertura.xml]

Les seuils d'emoji reproduisent ceux de gcovr (🟢 ≥ 90 %, 🟡 ≥ 75 %, 🔴 < 75 %,
⚫ sans donnée) pour rester cohérent avec le reste du rapport.
"""
import json
import re
import sys
from collections import OrderedDict

ANCHOR = "## 📄 File coverage"
HEADING = "## 📂 Couverture par domaine"
# Ancre de la note « écart Codecov » : le tableau Overall se termine juste avant
# la 1re section suivante ; on insère la note devant cette section.
NOTE_ANCHOR = HEADING


def emoji(covered, total):
    if total == 0:
        return "⚫"
    pct = 100.0 * covered / total
    if pct >= 90:
        return "🟢"
    if pct >= 75:
        return "🟡"
    return "🔴"


def cell(covered, total):
    pct = 0.0 if total == 0 else 100.0 * covered / total
    return f"{emoji(covered, total)} {covered}/{total} ({pct:.1f}%)"


def domain_of(filename):
    parts = filename.split("/")
    if len(parts) >= 2 and parts[0] == "src":
        return parts[1]
    return parts[0]


# Reproduit la classification par ligne de Codecov à partir du Cobertura de gcovr.
# gcovr émet chaque ligne comme <line number=.. hits=.. [branch="true"
# condition-coverage="P% (a/b)"]>. Codecov range chaque ligne dans un bucket :
#   - miss    : hits == 0
#   - partial : ligne de branchement exécutée mais dont TOUTES les branches ne
#               sont pas prises (a < b) — comptée « à part », PAS dans « covered »
#   - hit     : exécutée et (non branchante OU toutes branches prises)
# gcovr, lui, compte hit+partial dans « lines covered ». D'où l'écart d'en-tête.
_LINE_RE = re.compile(
    r'<line number="(\d+)" hits="(\d+)"'
    r'(?: branch="(true|false)")?'
    r'(?: condition-coverage="\d+% \((\d+)/(\d+)\)")?'
)
_CLASS_RE = re.compile(
    r'<class name="[^"]*" filename="([^"]+)"[^>]*>(.*?)</class>', re.S
)


def codecov_line_stats(xml_path):
    """(hit, partial, miss) façon Codecov, ou None si le XML est illisible.

    Dédoublonne par (fichier, n° de ligne) : gcovr émet chaque ligne deux fois
    (dans <method> puis au niveau <class>). Une ligne est « partial » si l'une
    de ses occurrences est une branche exécutée aux conditions incomplètes.
    """
    try:
        xml = open(xml_path, encoding="utf-8").read()
    except OSError:
        return None
    best = {}  # (fichier, ligne) -> (hits, cov, tot) agrégés
    for cm in _CLASS_RE.finditer(xml):
        fname = cm.group(1)
        for lm in _LINE_RE.finditer(cm.group(2)):
            lineno = int(lm.group(1))
            hits = int(lm.group(2))
            cov = int(lm.group(4)) if lm.group(4) else None
            tot = int(lm.group(5)) if lm.group(5) else None
            key = (fname, lineno)
            phits, pcov, ptot = best.get(key, (0, None, None))
            # Garde le plus d'info : hits max, et toute condition de branche vue.
            if cov is None:
                cov, tot = pcov, ptot
            best[key] = (max(hits, phits), cov, tot)
    if not best:
        return None
    hit = partial = miss = 0
    for hits, cov, tot in best.values():
        if hits == 0:
            miss += 1
        elif tot and cov is not None and cov < tot:
            partial += 1
        else:
            hit += 1
    return hit, partial, miss


def codecov_note(stats):
    """Bloc Markdown expliquant l'écart gcovr vs Codecov, à partir de stats."""
    hit, partial, miss = stats
    total = hit + partial + miss
    gcovr_cov = hit + partial  # ce que gcovr affiche dans « Lines »
    pct = lambda c: 0.0 if total == 0 else 100.0 * c / total
    return (
        "> ℹ️ **Pourquoi Codecov affiche un pourcentage plus bas ?** "
        "gcovr compte une ligne comme couverte dès qu'elle est exécutée au moins "
        "une fois — **branches partielles incluses**. Codecov range les lignes de "
        "branchement partiellement couvertes dans un bucket « partial » qu'il "
        "**ne compte pas** dans « covered ».\n"
        ">\n"
        f"> Ici **{partial} lignes partielles**. Équivalent « hits seuls » "
        f"(comme l'en-tête Codecov) : **{hit}/{total} ({pct(hit):.1f}%)** — "
        f"vs **{gcovr_cov}/{total} ({pct(gcovr_cov):.1f}%)** ci-dessus.\n\n"
    )


def main():
    json_path, md_path = sys.argv[1], sys.argv[2]
    xml_path = sys.argv[3] if len(sys.argv) > 3 else None
    data = json.load(open(json_path, encoding="utf-8"))

    domains = OrderedDict()
    for f in data.get("files", []):
        d = domains.setdefault(
            domain_of(f["filename"]),
            {"lc": 0, "lt": 0, "fc": 0, "ft": 0, "bc": 0, "bt": 0},
        )
        d["lc"] += f.get("line_covered", 0)
        d["lt"] += f.get("line_total", 0)
        d["fc"] += f.get("function_covered", 0)
        d["ft"] += f.get("function_total", 0)
        d["bc"] += f.get("branch_covered", 0)
        d["bt"] += f.get("branch_total", 0)

    rows = [
        HEADING,
        "",
        "| Domaine | Lines | Functions | Branches |",
        "|---------|-------|-----------|----------|",
    ]
    for name, d in sorted(domains.items()):
        rows.append(
            f"| **`src/{name}/`** | {cell(d['lc'], d['lt'])} "
            f"| {cell(d['fc'], d['ft'])} | {cell(d['bc'], d['bt'])} |"
        )
    section = "\n".join(rows) + "\n\n"

    md = open(md_path, encoding="utf-8").read()
    if ANCHOR in md:
        md = md.replace(ANCHOR, section + ANCHOR, 1)
    else:  # pas de section fichiers (rapport minimal) : on ajoute à la fin
        md = md.rstrip() + "\n\n" + section

    # Note « écart Codecov », insérée sous le tableau Overall (si le XML est là).
    stats = codecov_line_stats(xml_path) if xml_path else None
    if stats:
        note = codecov_note(stats)
        if NOTE_ANCHOR in md:
            md = md.replace(NOTE_ANCHOR, note + NOTE_ANCHOR, 1)
        else:  # ni domaine ni fichiers : on ajoute la note à la fin
            md = md.rstrip() + "\n\n" + note

    open(md_path, "w", encoding="utf-8").write(md)


if __name__ == "__main__":
    main()
