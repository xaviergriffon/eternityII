#!/usr/bin/env python3
"""Garde-fou contre la dérive entre les listes de sources du makefile et de CMakeLists.txt.

Le projet a DEUX systèmes de build qui décrivent les mêmes binaires : le makefile
(référence, exercé par `make test` et par toute la CI) et `CMakeLists.txt`
(confort IDE, jamais exercé par la CI). Rien ne les reliait : ajouter une suite
de tests au makefile sans la reporter dans CMake ne casse rien de visible… tant
que personne ne configure le projet avec CMake. C'est exactement ce qui est
arrivé — `run_tests_16` ne se liait plus, sur une dizaine de symboles manquants
(best_board_suite, cross_mask_suite, build_search_parts…).

Ce script compare, ensemble par ensemble, les listes homonymes des deux
fichiers et sort en erreur à la moindre divergence :

| makefile             | CMakeLists.txt       | binaire concerné            |
|----------------------|----------------------|-----------------------------|
| `OBJS`               | `PROD_SRCS`          | `eternityII`, `eternityII16`|
| `TEST_RUNNER`        | `TEST_RUNNER`        | run_tests_16 / run_tests_256|
| `TEST_SUITES_COMMON` | `TEST_SUITES_COMMON` | run_tests_16 / run_tests_256|
| `TEST_SOLUTION16`    | `TEST_SOLUTION16`    | run_tests_16                |
| `TEST_MODULES`       | `TEST_MODULES`       | run_tests_16 / run_tests_256|

La comparaison porte sur des ENSEMBLES : l'ordre des sources n'a aucune
incidence sur le link, et imposer un ordre identique n'apporterait que du bruit.

Usage : python3 tests/tools/check_build_lists.py [racine_du_dépôt]
Sortie : 0 si les listes coïncident, 1 sinon (avec le détail des écarts).
"""

import re
import sys
from pathlib import Path

# Objets du makefile qui n'ont pas d'équivalent dans PROD_SRCS de CMake :
# main.c est passé directement à add_executable (PROD_SRCS existe justement
# pour être réutilisé SANS main), et gpu_pruner.o est conditionnel (CUDA=1),
# côté CMake via ${CUDA_APP_SRC}.
OBJS_HORS_PERIMETRE = {"src/app/main.c"}

# Variables du makefile substituées « à la main » : leur valeur dépend d'une
# option de build, et les deux fichiers font le même choix par défaut.
SUBSTITUTIONS_MAKEFILE = {
    "$(LOGGER_OBJ)": "logger.o",  # NCURSES=0 ; CMake : ${LOGGER_SRC} → src/ui/logger.c
    "$(CUDA_OBJ)": "",            # vide sauf CUDA=1
    "$(BUILD_DIR)": "build",
}

SUBSTITUTIONS_CMAKE = {
    "${LOGGER_SRC}": "src/ui/logger.c",  # NCURSES=OFF
}


def lit_variable_makefile(texte: str, nom: str) -> set[str]:
    """Extrait `NOM := valeurs` d'un makefile, continuations `\\` comprises."""
    # Reconstitution ligne à ligne plutôt qu'une regex multi-lignes : le makefile
    # coupe ses listes avec `\` et intercale des commentaires entre les blocs.
    lignes = texte.splitlines()
    valeur: list[str] = []
    for i, ligne in enumerate(lignes):
        if not re.match(rf"^{re.escape(nom)}\s*:?=", ligne):
            continue
        courante = ligne.split("=", 1)[1]
        j = i
        while courante.rstrip().endswith("\\"):
            courante = courante.rstrip()[:-1]
            valeur.append(courante)
            j += 1
            courante = lignes[j] if j < len(lignes) else ""
        valeur.append(courante)
        break
    else:
        raise SystemExit(f"makefile : variable {nom} introuvable")
    brut = " ".join(valeur)
    return {jeton for jeton in brut.split() if jeton}


def lit_variable_cmake(texte: str, nom: str) -> set[str]:
    """Extrait `set(NOM ...)` d'un CMakeLists.txt, commentaires retirés.

    Le contenu est délimité par équilibrage de parenthèses, et non par une
    regex : les deux formes (une ligne, ou un bloc terminé par `)` seul) se
    lisent ainsi de la même façon.
    """
    debut = re.search(rf"^set\({re.escape(nom)}\b", texte, re.MULTILINE)
    if debut is None:
        raise SystemExit(f"CMakeLists.txt : variable {nom} introuvable")
    i = debut.end()
    profondeur = 1
    contenu = []
    while i < len(texte) and profondeur > 0:
        caractere = texte[i]
        if caractere == "#":  # commentaire : jusqu'à la fin de la ligne
            i = texte.find("\n", i)
            if i == -1:
                break
            continue
        if caractere == "(":
            profondeur += 1
        elif caractere == ")":
            profondeur -= 1
            if profondeur == 0:
                break
        contenu.append(caractere)
        i += 1
    if profondeur != 0:
        raise SystemExit(f"CMakeLists.txt : set({nom} ...) non refermé")
    return {jeton for jeton in "".join(contenu).split() if jeton}


def normalise_objs(objs: set[str]) -> set[str]:
    """`$(BUILD_DIR)/core/part.o` → `src/core/part.c`, hors variables vides."""
    sources = set()
    for objet in objs:
        for cle, valeur in SUBSTITUTIONS_MAKEFILE.items():
            objet = objet.replace(cle, valeur)
        objet = objet.strip()
        if not objet:
            continue
        if not objet.endswith(".o"):
            raise SystemExit(f"makefile : objet inattendu dans OBJS : {objet!r}")
        chemin = "src/" + objet[len("build/"):-len(".o")] + ".c"
        sources.add(chemin)
    return sources - OBJS_HORS_PERIMETRE


def normalise_cmake(jetons: set[str]) -> set[str]:
    resultat = set()
    for jeton in jetons:
        resultat.add(SUBSTITUTIONS_CMAKE.get(jeton, jeton))
    return resultat


def compare(etiquette: str, cote_make: set[str], cote_cmake: set[str]) -> list[str]:
    erreurs = []
    manquants = sorted(cote_make - cote_cmake)
    en_trop = sorted(cote_cmake - cote_make)
    if manquants:
        erreurs.append(
            f"{etiquette} : absent(s) de CMakeLists.txt mais présent(s) dans le makefile :\n"
            + "".join(f"    + {m}\n" for m in manquants)
        )
    if en_trop:
        erreurs.append(
            f"{etiquette} : présent(s) dans CMakeLists.txt mais absent(s) du makefile :\n"
            + "".join(f"    - {e}\n" for e in en_trop)
        )
    return erreurs


def main() -> int:
    racine = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    # Le dépôt track `makefile` en minuscule (cf. AGENTS.md) ; sur un système de
    # fichiers sensible à la casse, `Makefile` n'existe pas.
    makefile = (racine / "makefile").read_text()
    cmake = (racine / "CMakeLists.txt").read_text()

    erreurs: list[str] = []

    erreurs += compare(
        "OBJS (makefile) / PROD_SRCS (CMake)",
        normalise_objs(lit_variable_makefile(makefile, "OBJS")),
        normalise_cmake(lit_variable_cmake(cmake, "PROD_SRCS")),
    )

    for nom in ("TEST_RUNNER", "TEST_SUITES_COMMON", "TEST_SOLUTION16", "TEST_MODULES"):
        erreurs += compare(
            nom,
            lit_variable_makefile(makefile, nom),
            normalise_cmake(lit_variable_cmake(cmake, nom)),
        )

    if erreurs:
        sys.stderr.write(
            "Dérive entre le makefile (référence) et CMakeLists.txt :\n\n"
            + "\n".join(erreurs)
            + "\nReportez la liste du makefile dans CMakeLists.txt (ou l'inverse si\n"
              "c'est CMake qui a raison) — cf. docs/tests_et_ci.md.\n"
        )
        return 1

    print("makefile et CMakeLists.txt : listes de sources en phase.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
