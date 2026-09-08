/**
 * @file border_ring_dp.h
 * @brief Comptage exact de la masse totale des anneaux de bordure par
 * programmation dynamique sur des CLASSES de pièces interchangeables.
 *
 * `border_walk.c` compte en distinguant chacune des `BORDER_RING_LEN`
 * pièces de bord individuellement (un DFS aveugle, un bit par pièce dans
 * `b_faceused`) — correct, mais explose combinatoirement sur le vrai jeu
 * 256 pièces (des dizaines de milliards de nœuds pour une seule partition
 * sur 270, cf. docs/tests_et_ci.md) parce que la face intérieure d'une
 * pièce de bord n'est JAMAIS vérifiée par ce DFS (elle reste wildcard, cf.
 * `border_walk_count`) : deux pièces de bord partageant les deux mêmes
 * couleurs "anneau" mais une couleur intérieure différente sont donc
 * strictement interchangeables pour ce comptage, et le DFS aveugle les
 * retraite pourtant comme des branches distinctes à chaque case où l'une
 * d'elles pourrait convenir.
 *
 * Ce module regroupe les pièces de bord en classes d'équivalence (paire
 * ORDONNÉE couleur-requise/couleur-produite + forme coin/bord — voir le
 * commentaire de `bd_build_classes` dans `border_ring_dp.c` pour pourquoi
 * l'ordre est essentiel, pas juste un détail) et calcule, NIVEAU PAR NIVEAU
 * (une position de l'anneau à la fois, jamais les 59 en une seule table),
 * le nombre de façons d'atteindre chaque état (couleur requise, compteurs
 * restants PAR CLASSE) — au lieu d'un masque de bits par pièce réelle, et
 * au lieu de mémoïser toutes les positions ensemble. Ne garder que le
 * niveau courant + le niveau en construction (jamais les positions déjà
 * consommées) est le vrai levier mémoire ; regrouper les pièces en classes
 * est ce qui rend chaque niveau représentable du tout.
 *
 * Le total réel (permutations des pièces réelles au sein d'une classe) est
 * obtenu automatiquement : chaque transition pondère par le nombre de
 * pièces de cette classe encore disponibles, exactement comme un DFS réel
 * essaierait chacune séparément.
 *
 * La transition d'un niveau assez gros vers le suivant est parallélisée par
 * forks (`nb_workers`, cf. `bd_transition_parallel` dans `border_ring_dp.c`) :
 * les états du niveau courant, déjà calculé et immuable, sont indépendants
 * les uns des autres — chaque worker traite une plage disjointe et écrit son
 * niveau-suivant local dans un fichier temporaire, fusionné par le parent.
 *
 * Au-delà du budget RAM donné (`bd_split_threshold_bytes`, fixé par
 * `border_ring_dp_set_max_ram_mo` — `--dp-max-ram-mo`, obligatoire dès que
 * `--dp` est utilisé), un niveau est scindé en K fragments sur disque
 * (partitionnement externe par hachage de la clé, `struct bd_shard_set`,
 * `bd_level_to_shards`, forké) — un seul repris IMMÉDIATEMENT en mémoire (la
 * progression continue sans interruption), les K-1 autres empilés (pile LIFO
 * `struct bd_pending_stack`) pour être repris plus tard, chacun depuis la
 * position où il a été mis de côté. Contrairement à un mécanisme antérieur
 * (mode disque permanent, tout un niveau réécrit/relu à CHAQUE position tant
 * qu'il restait trop gros), un fragment mis de côté n'est écrit qu'une fois
 * et relu qu'une fois, jamais retouché entre les deux — voir le commentaire
 * de tête de la section « Scission par pile LIFO » dans `border_ring_dp.c`
 * pour le détail et le gain d'E/S mesuré.
 */
#ifndef eternityII_border_ring_dp_h
#define eternityII_border_ring_dp_h

#include "core/part.h"

/**
 * @brief Calcule la masse totale des anneaux de bordure valides, EXACTEMENT
 * (même définition et même résultat que `border_walk_count`), par
 * programmation dynamique sur les classes de pièces interchangeables.
 *
 * Aucun voisin n'est encore posé à la case d'ouverture `(0,0)` : comme
 * `border_walk_count`, cette fonction explore donc les candidats d'ouverture
 * possibles à cette case (chaque pièce de coin candidate est identifiée
 * individuellement, jamais regroupée en classe — sa pièce de coin "jumelle"
 * au sein d'une même classe ne présente pas forcément la même couleur de
 * fermeture, cf. le commentaire de `bd_count_openings` dans
 * `border_ring_dp.c`). Contrairement à `border_walk_count`, le DP ne rejoue
 * PAS le calcul complet pour chaque candidat : par symétrie de rotation à
 * 90° du plateau, toute bordure valide utilise nécessairement l'ensemble des
 * pièces-coin candidates, une fois chacune, donc leurs totaux individuels
 * sont rigoureusement égaux — un seul est calculé, puis multiplié par le
 * nombre de candidats (cf. le commentaire de `bd_count_openings`).
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`) —
 *                         utilisée uniquement pour énumérer les candidats
 *                         d'ouverture à `(0,0)`, exactement comme
 *                         `border_walk_count`.
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param nb_workers       Nombre de process forkés pour la transition d'un
 *                         niveau assez gros (`BD_FORK_MIN_STATES`) vers le
 *                         suivant. `1` désactive tout fork (mono-process,
 *                         chemin utilisé par les tests unitaires).
 * @return                 La masse totale — identique à ce que
 *                         `border_walk_count(map, all_rotate_parts, NULL, NULL)`
 *                         retournerait, mais sans jamais matérialiser
 *                         d'anneau complet.
 */
long long border_ring_count_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers);

/**
 * @brief Change le répertoire des fragments de scission et des fichiers
 * temporaires de la parallélisation par forks (`/tmp` par défaut) — comme
 * `--stock-spill-dir` pour le stock principal (`core/stock_spill.c`).
 *
 * `/tmp` est souvent une petite partition ou un tmpfs plafonné bien en-deçà
 * de la RAM de la machine, indépendamment de sa taille — observé en
 * pratique, `/tmp` saturé sur une machine par ailleurs bien dotée
 * (2x10 cœurs/48 Go), cf. docs/tests_et_ci.md.
 *
 * @param dir Chemin du répertoire — doit rester valide pendant toute la durée
 *            de l'appel à `border_ring_count_dp` qui suit (aucune copie
 *            interne n'en est faite, comme `argv` ne l'est jamais non plus).
 */
void border_ring_dp_set_spill_dir(const char *dir);

/**
 * @brief Fixe le budget mémoire dédié à UN NIVEAU (ou une tranche reprise
 * depuis la pile, cf. le commentaire de tête du fichier) de la DP —
 * obligatoire dès que `--dp` est utilisé (cf. `border_mass.c`,
 * `--dp-max-ram-mo`), remplace les anciennes constantes calées à la main sur
 * une seule machine (2 Go de seuil de scission, 768 Mo de cible de fragment).
 *
 * Pilote deux seuils : `bd_split_threshold_bytes` (déclenche une scission dès
 * qu'un niveau dépasse ce budget) prend directement la valeur donnée ;
 * `bd_shard_target_bytes` (taille cible d'un fragment) en est dérivée —
 * `budget / (nb_workers * 3)`, avec un plancher bas — pour que
 * `nb_workers` fragments simultanés pendant une compaction restent,
 * ensemble, sous ce même budget.
 *
 * @param mo         Budget en Mo. Doit rester valide pour tout l'appel à
 *                   `border_ring_count_dp` qui suit.
 * @param nb_workers Nombre de process forkés (même valeur que celle passée à
 *                   `border_ring_count_dp`) — utilisé pour dimensionner
 *                   `bd_shard_target_bytes`.
 */
void border_ring_dp_set_max_ram_mo(long mo, int nb_workers);

#endif /* eternityII_border_ring_dp_h */
