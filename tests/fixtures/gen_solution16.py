#!/usr/bin/env python3
"""Génère l'en-tête C `solution16.h` à partir d'une solution 4×4 figée en JSON.

Entrée  : un fichier JSON au format produit par `print_possibility_packet`
          (`{"alloc": .., "x": .., "y": .., "grid": [[..], ..]}`), où `grid` est
          parcouru ligne par ligne en y (extérieur) puis x (intérieur), chaque
          valeur valant `id + ETERN_PARTS*rotation` de la pièce posée.
Sortie  : sur stdout, un en-tête C définissant la grille en indexation [x][y]
          (celle de `struct possibility_packet.grid`), prête à recopier dans un
          paquet « golden » côté test. `b_faceused` n'est PAS émis : il est
          recalculé dans le test via `set_face_used`.

Usage   : python3 gen_solution16.py solution16.json > solution16.h

Ce script n'a aucune dépendance externe (json stdlib) — cf. coverage_by_domain.py.
"""
import json
import sys


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: %s <solution16.json>\n" % argv[0])
        return 2

    with open(argv[1], "r", encoding="utf-8") as fp:
        data = json.load(fp)

    alloc = int(data["alloc"])
    pos_x = int(data.get("x", 0))
    pos_y = int(data.get("y", 0))
    grid_json = data["grid"]  # indexé [y][x], valeur = grid[x][y]

    size = len(grid_json)
    for row in grid_json:
        if len(row) != size:
            sys.stderr.write("error: grille non carrée\n")
            return 1

    # Transpose en [x][y] pour coller à possibility_packet.grid[x][y].
    grid_xy = [[grid_json[y][x] for y in range(size)] for x in range(size)]

    out = sys.stdout
    out.write("/* GÉNÉRÉ par tests/fixtures/gen_solution16.py — NE PAS ÉDITER.\n")
    out.write(" * Régénéré au build (cible make test-16 / coverage-16) depuis\n")
    out.write(" * tests/fixtures/solution16.json (un plateau 4×4 réellement résolu). */\n")
    out.write("#ifndef ETII_TESTS_SOLUTION16_H\n")
    out.write("#define ETII_TESTS_SOLUTION16_H\n\n")
    out.write("#include <stdint.h>\n\n")
    out.write("#define SOLUTION16_SIZE  %d\n" % size)
    out.write("#define SOLUTION16_ALLOC %d\n" % alloc)
    out.write("#define SOLUTION16_X     %d\n" % pos_x)
    out.write("#define SOLUTION16_Y     %d\n\n" % pos_y)
    out.write("/* Indexé [x][y], comme struct possibility_packet.grid. */\n")
    out.write("static const int16_t SOLUTION16_GRID[SOLUTION16_SIZE][SOLUTION16_SIZE] = {\n")
    for x in range(size):
        cells = ", ".join("%d" % v for v in grid_xy[x])
        out.write("    { %s },\n" % cells)
    out.write("};\n\n")
    out.write("#endif /* ETII_TESTS_SOLUTION16_H */\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
