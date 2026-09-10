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
  plus à terminer sur le jeu réel — une seule partition peut dépasser
  33 milliards d'anneaux trouvés sans se clore, faute d'heuristique
  d'élagage (le walker n'utilise ni MRV ni forward-check, par choix).
- **`--dp`** (`tests/tools/border_ring_dp.c`) : algorithme différent et exact
  — regroupe les pièces de bord interchangeables en classes (couleur
  d'entrée/sortie ordonnée) et calcule niveau par niveau le nombre de façons
  d'atteindre chaque état. Une seule pièce-coin d'ouverture est développée
  (les 4 sont rigoureusement équivalentes par symétrie de rotation), le
  résultat est multiplié par 4. Combinable avec `--forks` (une transition de
  niveau se parallélise par plage d'indices) et avec `--dp-max-ram-mo` (un
  coordinateur à deux modes SOLO/POOL borne la RAM réelle — pas seulement
  la taille nominale d'un niveau — en scindant les niveaux trop gros en
  fragments sur une pile LIFO partagée, rechargés avec une marge RAM exacte
  en mode POOL et heuristique en mode SOLO) et avec `--spill-dir` pour
  rediriger les fragments vers un disque plus grand que `/tmp`. Le contrôle
  de budget se fait désormais aussi **pendant** la construction d'un niveau
  (« pause mi-transition »), pas seulement une fois un niveau complet —
  corrigé après un OOM de production (`--dp-max-ram-mo 30000 --forks 10`,
  2026-09-10 : jobs à 3-8,5 Gio de RSS réel contre un budget nominal par job
  de 1,5 Gio) où le contrôle ne s'exécutait qu'une fois par position, trop
  tard pour empêcher le pic mémoire réel — voir docs/tests_et_ci.md §
  Scission par pile LIFO pour le détail (mécanisme, piège de livelock trouvé
  et corrigé sous test, tests dédiés).
- **`--save-rings FILE --max-rings N`** (avec `--dp`) : reconstruit les
  anneaux réels et les écrit en `.back` — passe avant persistée, tables de
  complétion calculées bottom-up, DFS guidé sur les classes (jamais sur les
  pièces brutes), puis expansion en assignations de pièces réelles. Un run de
  test avait produit 8 anneaux pour l'ouverture unique calculée (32 au
  total) — **ce chiffre est un échantillon borné par `--max-rings`, pas la
  masse réelle** : sur le jeu réel (256 pièces), la masse totale exacte
  (`--dp` sans `--save-rings`) dépasse la capacité d'un `long long` signé
  (> 9,2x10^18), au point qu'un seul fragment fermé pouvait déjà déborder —
  corrigé le 2026-09-10 par le passage à `bd_ring_count_t` (`unsigned
  __int128`, voir `border_ring_dp.h`). À revérifier/rechiffrer avec ce
  correctif avant de citer un total définitif.

## Arbitrages qui restent valables

- **Fork, jamais threads**, à chaque étage de parallélisme (marche
  historique du projet) : isolation mémoire et de panne entre workers,
  aucune synchronisation partagée à auditer.
- **Pile LIFO** pour les fragments en attente : borne la profondeur par le
  nombre de positions de l'anneau, pas par la largeur de l'espace d'états.
- **Aucune perte de fragment sur refus RAM** : un job qui ne peut être admis
  attend qu'un slot se libère, jamais un abandon silencieux — même principe
  que `expand_datas_to_level` (AGENTS.md § RAM cap).

## Pistes écartées, avec preuve

- Optimisation meet-in-the-middle (approche C) pour le DFS séquentiel : non
  nécessaire, `--dp` a changé d'algorithme plutôt que d'optimiser le DFS.
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
