#!/usr/bin/env python3
"""Insère une section « Couverture par domaine » dans le rapport gcovr Markdown.

Lit le résumé JSON de gcovr (`--json-summary`, une entrée par fichier), agrège
les lignes / fonctions / branches par domaine (la 2e composante du chemin, p. ex.
`src/core/...` -> `core`), puis insère un tableau Markdown dans `coverage.md`
juste avant la section « ## 📄 File coverage » produite par gcovr.

Usage : coverage_by_domain.py <summary.json> <coverage.md>

Les seuils d'emoji reproduisent ceux de gcovr (🟢 ≥ 90 %, 🟡 ≥ 75 %, 🔴 < 75 %,
⚫ sans donnée) pour rester cohérent avec le reste du rapport.
"""
import json
import sys
from collections import OrderedDict

ANCHOR = "## 📄 File coverage"
HEADING = "## 📂 Couverture par domaine"


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


def main():
    json_path, md_path = sys.argv[1], sys.argv[2]
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
    open(md_path, "w", encoding="utf-8").write(md)


if __name__ == "__main__":
    main()
