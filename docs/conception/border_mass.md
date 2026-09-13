# `border_mass` — masse totale des anneaux de bordure

**Statut** : implémenté — ce document est un résumé, la référence de
comportement à jour est [docs/tests_et_ci.md](../tests_et_ci.md) (sections
« Outil `border_mass` » et « `--dp` : comptage exact par programmation
dynamique sur classes de pièces »).

## Objectif

Calculer la masse totale des anneaux de bordure valides (les
`BORDER_RING_LEN` cases du pourtour du plateau, 60 sur le puzzle 256 pièces)
et, une fois ce chiffre jugé exploitable, reconstruire les anneaux réels
comme racines `.back` injectables dans le stock normal.

## État actuel

- **DFS séquentiel** (`tests/tools/border_walk.c`/`border_mass.c`) : une
  recherche exhaustive ancrée au coin `(0,0)` retrouve d'elle-même toute la
  population d'anneaux — aucun voisin n'est posé à la toute première case,
  donc n'importe lequel des 4 coins peut ouvrir la recherche ; le nombre brut
  trouvé **est** déjà la masse totale, sans multiplication externe. Ne
  termine pas en temps raisonnable sur le jeu réel (256 pièces).
- **Parallélisation par forks** (`--forks N`, ordre « coins d'abord » +
  expansion en largeur de la frontière) : accélère mais ne suffit pas non
  plus à terminer sur le jeu réel, faute d'heuristique d'élagage (le walker
  n'utilise ni MRV ni forward-check, par choix). Mesure du 13/09/2026,
  43 s sur 8 workers : 6,4 × 10⁹ nœuds, **6,4 × 10⁸ anneaux fermés**
  (un anneau tous les ~10 nœuds). Ne pas confondre « ne termine pas » et
  « ne produit rien » : c'est ce débit qui rend l'échantillonnage viable.
- **`--dp`** (`tests/tools/border_ring_dp.c`) : algorithme différent et exact
  — regroupe les pièces de bord interchangeables en classes (couleur
  d'entrée/sortie ordonnée) et calcule niveau par niveau le nombre de façons
  d'atteindre chaque état. Une seule pièce-coin d'ouverture est développée
  (les 4 sont rigoureusement équivalentes par symétrie de rotation à 90°), le
  résultat est multiplié par 4. Combinable avec `--forks` (une transition de
  niveau se parallélise par plage d'états), `--dp-max-ram-mo` (taille du
  tampon de tri) et `--spill-dir`.
- **Tri externe** (2026-09-12) : un niveau est un TABLEAU TRIÉ, pas une table
  de hachage ; un niveau trop gros pour la RAM devient un FICHIER trié, lu
  séquentiellement par la position suivante. Remplace la scission en
  fragments repris indépendamment, qui perdait toute fusion des états entre
  fragments dès la position suivant la scission et faisait dégénérer la DP en
  somme de sous-DP redondantes (mesures : docs/tests_et_ci.md § Tri externe).
  Un état tient sur 64 bits et coûte 24 octets stocké, contre 111 avant.

## Où en est le calcul, et ce que ça change

Un run de production de 27 h (`--dp-max-ram-mo 35000 --forks 10`) sur la
version à fragments n'avait accumulé que **1,01 × 10²⁷** — pour une masse
réelle estimée à **3,80 × 10³⁷ ± 6 %**, soit 3 × 10⁻⁹ % du total.

Cette estimation vient d'une voie **totalement indépendante du DP**, validée
en retrouvant exactement le `4` de `data/pieces16.csv` : les 60 pièces de
bord forment un multigraphe dirigé sur les 5 couleurs de bordure,
parfaitement équilibré (12 entrantes / 12 sortantes par couleur), et un
anneau est exactement un circuit eulérien de ce graphe. Le **théorème BEST**
en donne le compte exact en quelques microsecondes —
`ec = tw × ∏(deg−1)! = 3432 × (11!)⁵ = 3,478 × 10⁴¹` — et un échantillonnage
uniforme de circuits eulériens donne la fraction de ceux dont les 4 coins
tombent aux positions 0/15/30/45 (2,73 × 10⁻⁵ sur 9 M tirages).

Conséquence pour le sous-projet 2 (reconstruire les anneaux comme racines
`.back`) : avec ~10³⁷ anneaux, la population n'est pas *exhaustible* comme
jeu de racines — `--max-rings 10⁹` en échantillonnerait 10⁻²⁸. Le chiffre
exact garde un intérêt propre, mais la décision qu'il devait éclairer est
déjà tranchée par sa borne.

## Échantillonner plutôt que reconstruire (13/09/2026)

`--save-rings` a d'abord été câblé sur `--dp` seul, au motif que « le DFS
brut est trop lent pour reconstruire les anneaux réels ». Vrai pour
l'énumération EXHAUSTIVE, faux pour un échantillon — et c'est l'échantillon
qu'on veut. Le refus a été levé : `--save-rings` sans `--dp` produit
**10⁶ anneaux en 0,94 s** (8 forks), là où la voie `--dp` demande ~31 h et
~537 Go avant de livrer le premier (un run de production a été arrêté à
18 h 41, position 26/59 de sa deuxième passe avant, `rings.bin` encore vide).

Attention à ne pas surestimer ce débit, comme la première rédaction de cette
section le faisait : le walker FERME ~15 M anneaux/s, mais en écrire est 17×
plus lent — **~1 M/s, ~500 Mo/s, borné par le disque**. Le chiffre à citer
pour `--save-rings` est le second.

Deux mesures ont écarté les objections à cet échantillon :

- **« les anneaux se ressembleront tous »** — faux : sur 10⁶ anneaux
  consécutifs, 10⁶ **frontières intérieures distinctes**, zéro doublon. La
  classe d'une pièce de bord ne retient que ses deux faces adjacentes sur
  l'anneau ; sa face intérieure — la seule que voit la recherche intérieure —
  n'y entre pas.
- **« le motif de couleurs, lui, se répète »** — vrai (~500 anneaux par motif
  de classes en séquentiel, ~17 en étalant sur 270 partitions), mais c'est
  une métrique portant sur des faces que la recherche intérieure ne consulte
  jamais. Elle a été mesurée puis écartée comme critère : ne pas la
  reproposer sans revenir à ce paragraphe.

La conséquence sur l'outil : `--max-rings` borne désormais le TRAVAIL et pas
seulement la sortie, via une valeur de retour d'arrêt sur
`border_ring_found_cb` qui remonte `bw_dfs`, l'expansion de frontière et la
boucle des coins d'ouverture du DP.

## Arbitrages qui restent valables

- **Fork, jamais threads**, à chaque étage de parallélisme (marche
  historique du projet) : isolation mémoire et de panne entre workers,
  aucune synchronisation partagée à auditer. Le tri externe rend ce choix
  gratuit : un worker rend un fichier DÉJÀ TRIÉ, donc la remise au parent est
  une fusion linéaire — alors qu'elle coûtait la transition entière tant que
  le parent devait réinsérer dans une table de hachage.
- **Aucune perte de donnée sur dépassement du budget RAM** : le trieur déverse
  un run trié et poursuit, jamais un abandon silencieux — même principe que
  `expand_datas_to_level` (AGENTS.md § RAM cap).

## Pistes écartées, avec preuve

- Optimisation meet-in-the-middle (approche C) pour le DFS séquentiel : non
  nécessaire, `--dp` a changé d'algorithme plutôt que d'optimiser le DFS.
- **Scission d'un niveau en fragments repris indépendamment** (pile LIFO,
  coordinateur SOLO/POOL, pause mi-transition) : correcte mais catastrophique
  — le partitionnement par hachage ne sépare aucun doublon au moment de la
  scission, mais dès la position suivante deux fragments produisent les mêmes
  clés sans plus jamais les fusionner. Remplacée par le tri externe, qui
  garde le niveau entier. Ne pas y revenir : chiffres à l'appui dans
  docs/tests_et_ci.md § Tri externe.
- Table de mémoïsation globale (toutes les positions dans une seule table) :
  un état à la position P ne dépend que du niveau P+1 — remplacée par une
  structure à deux niveaux (courant + suivant), qui borne le pic mémoire à
  la position la plus chargée plutôt qu'à la somme des 59.
- Regroupement par paire de couleurs non ordonnée : donnait un résultat 32×
  trop grand (deux pièces peuvent partager les deux mêmes couleurs avec une
  orientation opposée, donc ne pas être interchangeables) — corrigé en
  gardant l'ordre entrée/sortie dans la clé de classe.
- Fork imbriqué (un job du pool qui re-forke ses propres sous-workers) :
  écarté, la comptabilité de cœurs partagée qu'il faudrait n'est pas
  justifiée face au gain.
