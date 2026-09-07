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
 * Au-delà d'une taille de niveau généreuse (`bd_disk_mode_min_bytes`, 2 Go
 * par défaut — mesuré insuffisant même sur une machine à 48 Go de RAM sans
 * ce mécanisme), un niveau bascule d'une table unique en mémoire vers K
 * fragments sur disque (partitionnement externe par hachage de la clé,
 * `struct bd_shard_set`), chacun assez petit pour tenir large en mémoire.
 * Chaque transition devient alors deux vagues de forks (éclatement puis
 * compactage, cf. `bd_transition_disk`/`bd_compact_dir`) au lieu d'une
 * seule : le pic mémoire devient `nb_workers` fragments simultanés,
 * indépendant de la taille totale du niveau — voir le commentaire de tête de
 * la section « Mode disque » dans `border_ring_dp.c` pour le détail.
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
 * `border_walk_count`, cette fonction explore donc déjà les 4 coins
 * possibles comme point d'ouverture (chaque pièce de coin candidate est
 * essayée individuellement, jamais regroupée en classe — sa pièce de coin
 * "jumelle" au sein d'une même classe ne présente pas forcément la même
 * couleur de fermeture, cf. le commentaire de `bd_count_openings` dans
 * `border_ring_dp.c`) — pas de ×4 supplémentaire à appliquer en aval, comme
 * pour `border_walk_count`.
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

#endif /* eternityII_border_ring_dp_h */
