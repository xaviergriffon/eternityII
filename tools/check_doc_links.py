#!/usr/bin/env python3
"""
Vérifie les liens Markdown INTERNES du dépôt : la cible existe-t-elle, et si le
lien porte une ancre (`fichier.md#ancre`), cette ancre correspond-elle encore à
un titre du fichier visé ?

Le besoin : les titres sont étendus au fil du temps (« ## Canal de contrôle (v9) »
devient « ## Canal de contrôle (v9, étendu en v10 et v12) ») sans que les renvois
suivent. L'ancre pointe alors dans le vide — GitHub n'affiche aucune erreur, il
ouvre simplement le haut du fichier, donc la dérive est invisible à la relecture.

Deux catégories de défaut sont distinguées, parce qu'elles ne se réparent pas de
la même façon :
  - ancre absente  : le fichier est là, le titre a bougé  → repointer sur l'ancre réelle
  - fichier absent : la cible a été supprimée/renommée    → `git log --diff-filter=D` puis repointer

L'algorithme d'ancre reproduit celui de GitHub (github-slugger) : minuscules,
suppression de la ponctuation SAUF le tiret et le souligné, espaces -> tirets,
et suffixe `-1`, `-2`… pour les titres homonymes dans un même fichier.

Usage :
    python3 tools/check_doc_links.py [fichier.md ...]   # défaut : tout le dépôt
"""

import re
import sys
import unicodedata
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Répertoires sans intérêt documentaire (et potentiellement énormes).
SKIP_DIRS = {".git", "build", "node_modules", ".claude"}

# ──────────────────────────────────────────────────────────────────────────────
# Ancres façon GitHub
# ──────────────────────────────────────────────────────────────────────────────

# Un caractère survit s'il est alphanumérique au sens Unicode (donc « é », « ô »,
# « π »…), un espace, un tiret ou un souligné. Tout le reste — ponctuation,
# accents combinants isolés, symboles, emoji — disparaît. C'est bien « supprimé »
# et non « remplacé par un tiret » : « (v9, étendu » donne « v9-étendu », pas
# « -v9--étendu ».
def _keep(ch: str) -> bool:
    if ch in "-_ ":
        return True
    return ch.isalnum()


_MD_LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")
_MD_IMAGE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
_HTML_TAG = re.compile(r"<[^>]+>")


def github_anchor(title: str) -> str:
    """Ancre GitHub d'un texte de titre (sans les `#` de niveau)."""
    # GitHub indexe le TEXTE rendu : une image disparaît, un lien garde son
    # libellé, une balise HTML s'efface.
    text = _MD_IMAGE.sub("", title)
    text = _MD_LINK.sub(r"\1", text)
    text = _HTML_TAG.sub("", text)
    text = text.strip().lower()
    text = "".join(ch for ch in text if _keep(ch))
    return text.replace(" ", "-")


_ATX = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
_FENCE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")
_HTML_ANCHOR = re.compile(r"""<a\s[^>]*\bname\s*=\s*["']([^"']+)["']""", re.I)
_HTML_ID = re.compile(r"""<[^>]*\bid\s*=\s*["']([^"']+)["']""", re.I)


def collect_anchors(path: Path) -> set:
    """Toutes les ancres adressables d'un fichier Markdown."""
    anchors = set()
    seen = {}
    fence = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _FENCE.match(line)
        if m:
            # Un bloc de code s'ouvre et se ferme sur la même famille de clôture ;
            # un `##` à l'intérieur est du texte, pas un titre.
            if fence is None:
                fence = m.group(1)[0]
            elif m.group(1)[0] == fence:
                fence = None
            continue
        if fence is not None:
            continue
        for explicit in _HTML_ANCHOR.findall(line) + _HTML_ID.findall(line):
            anchors.add(explicit.lower())
        m = _ATX.match(line)
        if not m:
            continue
        base = github_anchor(m.group(2))
        if not base:
            continue
        n = seen.get(base, 0)
        seen[base] = n + 1
        anchors.add(base if n == 0 else f"{base}-{n}")
    return anchors


# ──────────────────────────────────────────────────────────────────────────────
# Extraction des liens
# ──────────────────────────────────────────────────────────────────────────────

# [libellé](cible) et la forme référence [libellé]: cible.
# Le libellé peut enjamber une fin de ligne — plusieurs renvois du dépôt sont
# coupés en deux par le rehabillage du paragraphe ; un parseur ligne à ligne les
# rate en silence, ce qui est exactement le genre d'angle mort qu'on traque ici.
_LINK = re.compile(r"\[[^\[\]]*\]\(\s*<?([^)>\s]+)>?(?:\s+\"[^\"]*\")?\s*\)", re.S)
_REF_DEF = re.compile(r"^[ ]{0,3}\[[^\]\n]+\]:[ ]*<?(\S+)>?[ ]*$", re.M)
_INLINE_CODE = re.compile(r"`+[^`\n]*`+")


def _mask(text: str) -> str:
    """Remplace par des espaces tout ce qui n'est pas du Markdown actif.

    Les décalages (et donc les numéros de ligne) sont préservés caractère pour
    caractère : les blocs clôturés et le code en ligne deviennent du blanc, pas
    du vide. Sans cela, un `tab[i](j)` dans un exemple de code passerait pour un
    lien.
    """
    out = []
    fence = None
    for line in text.splitlines(keepends=True):
        m = _FENCE.match(line)
        if m:
            if fence is None:
                fence = m.group(1)[0]
            elif m.group(1)[0] == fence:
                fence = None
            out.append(" " * (len(line) - 1) + "\n" if line.endswith("\n") else " " * len(line))
            continue
        if fence is not None:
            out.append(" " * (len(line) - 1) + "\n" if line.endswith("\n") else " " * len(line))
            continue
        out.append(_INLINE_CODE.sub(lambda mm: " " * len(mm.group(0)), line))
    return "".join(out)


def iter_links(path: Path):
    """(numéro de ligne, cible brute) pour chaque lien hors bloc de code."""
    masked = _mask(path.read_text(encoding="utf-8", errors="replace"))
    for regex in (_LINK, _REF_DEF):
        for m in regex.finditer(masked):
            yield masked.count("\n", 0, m.start()) + 1, m.group(1)


def is_external(target: str) -> bool:
    return bool(re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target)) or target.startswith("//")


# ──────────────────────────────────────────────────────────────────────────────
# Vérification
# ──────────────────────────────────────────────────────────────────────────────


def markdown_files():
    for path in sorted(REPO_ROOT.rglob("*.md")):
        if any(part in SKIP_DIRS for part in path.relative_to(REPO_ROOT).parts):
            continue
        yield path


def check(paths):
    anchor_cache = {}
    missing_anchor = []
    missing_file = []

    for path in paths:
        rel = path.relative_to(REPO_ROOT)
        for lineno, raw in iter_links(path):
            if is_external(raw) or raw.startswith("#!"):
                continue

            file_part, _, anchor = raw.partition("#")
            # L'ancre est percent-encodée par certains éditeurs (%C3%B4 pour ô).
            anchor = _unquote(anchor).lower()

            if file_part:
                target = (path.parent / _unquote(file_part)).resolve()
            else:
                target = path  # lien purement interne au fichier

            if not target.exists():
                missing_file.append((rel, lineno, raw))
                continue
            if not anchor:
                continue
            if target.suffix.lower() != ".md":
                # Une ancre sur un .c/.py ne s'évalue pas (GitHub y accepte #L42).
                continue

            if target not in anchor_cache:
                anchor_cache[target] = collect_anchors(target)
            if anchor not in anchor_cache[target]:
                missing_anchor.append((rel, lineno, raw, target))

    return missing_anchor, missing_file, anchor_cache


def _unquote(text: str) -> str:
    from urllib.parse import unquote

    return unicodedata.normalize("NFC", unquote(text))


def _suggest(anchor: str, anchors: set, limit: int = 3):
    """Ancres existantes les plus proches, pour orienter la réparation."""
    import difflib

    return difflib.get_close_matches(anchor, sorted(anchors), n=limit, cutoff=0.4)


def main(argv):
    if argv:
        paths = [Path(a).resolve() for a in argv]
    else:
        paths = list(markdown_files())

    missing_anchor, missing_file, anchors = check(paths)

    if missing_file:
        print(f"FICHIER ABSENT ({len(missing_file)}) :")
        for rel, lineno, raw in missing_file:
            print(f"  {rel}:{lineno}: {raw}")
        print()

    if missing_anchor:
        print(f"ANCRE ABSENTE ({len(missing_anchor)}) :")
        for rel, lineno, raw, target in missing_anchor:
            anchor = _unquote(raw.partition("#")[2]).lower()
            hint = _suggest(anchor, anchors[target])
            print(f"  {rel}:{lineno}: {raw}")
            if hint:
                print(f"      candidats : {', '.join('#' + h for h in hint)}")
        print()

    total = len(missing_anchor) + len(missing_file)
    if total:
        print(
            f"{total} lien(s) cassé(s) : "
            f"{len(missing_anchor)} ancre(s) absente(s), "
            f"{len(missing_file)} fichier(s) absent(s)."
        )
        return 1

    print(f"OK — aucun lien interne cassé ({len(paths)} fichier(s) Markdown).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
