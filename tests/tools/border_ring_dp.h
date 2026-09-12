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
 * Chaque niveau est un TABLEAU TRIÉ par clé, sans doublon — pas une table de
 * hachage : les états identiques se retrouvent par l'ORDRE (tri puis fusion
 * des clés égales adjacentes), jamais par co-résidence en mémoire. Un niveau
 * qui dépasse le budget devient donc un FICHIER trié, lu séquentiellement
 * par la position suivante, et reste un niveau UNIQUE et intégralement
 * fusionné quelle que soit sa taille. C'est la différence de fond avec le
 * mécanisme précédent (scission en fragments repris indépendamment), qui
 * perdait toute fusion entre fragments dès la position suivant la scission
 * et faisait dégénérer la DP en somme de sous-DP redondantes — mesures et
 * chiffres du run de production qui l'a montré : commentaire de tête de
 * `border_ring_dp.c`.
 *
 * Un état tient dans une clé compacte : chaque compteur de classe est borné
 * par la multiplicité de SA classe, donc codé sur `ceil(log2(m+1))` bits
 * (59 bits en tout pour le jeu réel, compteurs et couleur requise compris —
 * mais 68 pour certaines fixtures de test, d'où une clé de 128 bits plutôt
 * que 64, cf. `bd_key_t` dans border_ring_dp.c). Un état stocké coûte
 * 32 octets contre 111 pour la version précédente (clé de 29 octets dans une
 * table de hachage tenue sous 60 % de charge).
 *
 * La transition d'un niveau assez gros vers le suivant est parallélisée par
 * forks (`nb_workers`, cf. `bd_transition_parallel` dans `border_ring_dp.c`) :
 * les états du niveau courant, déjà calculé et immuable, sont indépendants
 * les uns des autres — chaque worker traite une plage disjointe et rend UN
 * fichier déjà TRIÉ, que le parent fusionne en une passe linéaire. C'est ce
 * qui rend la parallélisation franche ici : la version précédente faisait
 * RÉINSÉRER au parent, une à une dans une table de hachage, la totalité de
 * la production de ses workers — une partie série valant ~100 % du travail
 * utile, donc un speedup borné à ~2 quel que soit `--forks`.
 *
 * Le budget mémoire (`border_ring_dp_set_max_ram_mo` — `--dp-max-ram-mo`,
 * obligatoire dès que `--dp` est utilisé) ne dimensionne QUE le tampon de
 * tri. Le dépasser ajoute un run trié à fusionner : jamais un fragment qui
 * cesserait de fusionner ses doublons avec les autres, et jamais un seuil
 * dérivé (`/4`, `/(workers*2)`, `/(workers*3)`) qui rendait le budget réel
 * de la version précédente 20 fois plus petit que celui demandé.
 */
#ifndef eternityII_border_ring_dp_h
#define eternityII_border_ring_dp_h

#include "core/part.h"
#include "tools/border_walk.h"

/**
 * @brief Type des "masses" d'anneaux (nombre de façons) manipulées par ce
 * module — `unsigned __int128`, pas `long long`.
 *
 * Sur le jeu réel (256 pièces), la masse totale dépasse le plafond d'un
 * `long long` signé (2^63-1 ≈ 9,223x10^18) : le compteur "fragment fermé,
 * +N anneaux" débordait silencieusement en négatif (UB en C, wrap en
 * pratique) avant l'introduction de ce type. Une borne haute jetable
 * (permuter librement 56 pièces de bord + 4 coins, sans aucune contrainte de
 * couleur) donne 56!x4! ≈ 1,7x10^76 — bien au-delà même d'un `unsigned
 * __int128` (~3,4x10^38) — donc AUCUN entier fixe n'est prouvablement
 * suffisant dans l'absolu. En pratique, la multiplicité par classe reste
 * petite (max 4 sur `data/pieces.csv`, cf. `bd_build_classes`), ce qui rend
 * un dépassement de 128 bits hautement improbable pour ce jeu de pièces —
 * mais `bd_ring_count_format`/l'addition et la multiplication protégées de
 * `border_ring_dp.c` existent précisément pour transformer un futur
 * dépassement (jeu de pièces différent, bordure plus permissive) en arrêt
 * bruyant plutôt qu'en un nouveau wrap silencieux.
 */
typedef unsigned __int128 bd_ring_count_t;

/** Taille minimale d'un buffer passé à `bd_ring_count_format` — 39 chiffres
 * décimaux pour la valeur maximale d'un `bd_ring_count_t` (2^128-1), plus le
 * terminateur nul. */
#define BD_RING_COUNT_STRLEN 40

/**
 * @brief Formate `value` en décimal dans `buf` (tronqué si `buflen` est trop
 * petit, jamais un débordement de `buf`) — seul moyen d'afficher un
 * `bd_ring_count_t`, `printf` n'ayant aucune conversion native pour
 * `__int128`. `buflen` doit être au moins `BD_RING_COUNT_STRLEN`.
 */
void bd_ring_count_format(bd_ring_count_t value, char *buf, size_t buflen);

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
bd_ring_count_t border_ring_count_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers);

/**
 * @brief Change le répertoire des runs déversés par le trieur, des tranches
 * triées rendues par les workers forkés et des niveaux trop gros pour la RAM
 * (`/tmp` par défaut) — comme `--stock-spill-dir` pour le stock principal
 * (`core/stock_spill.c`).
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
 * @brief Fixe la taille du TAMPON DE TRI de la DP — obligatoire dès que
 * `--dp` est utilisé (cf. `border_mass.c`, `--dp-max-ram-mo`).
 *
 * Un seul seuil, et rien n'en est dérivé : c'est la mémoire que le trieur
 * s'autorise avant de déverser un run trié sur disque. Le dépasser n'a
 * AUCUNE conséquence algorithmique — un run de plus à fusionner en fin de
 * position, le niveau reste entier et intégralement fusionné.
 *
 * Ne pas confondre avec le mécanisme précédent, où ce même nombre était
 * divisé quatre fois (`/4` en mode SOLO, `/(nb_workers*2)` par job du pool,
 * `/(nb_workers*3)` par fragment) pour servir de seuil de SCISSION : avec
 * `--dp-max-ram-mo 35000 --forks 10`, un job n'y disposait que de 1,75 Go,
 * et ajouter des workers fragmentait donc la DP plus tôt — la
 * parallélisation créait le problème qu'elle devait résoudre.
 *
 * Le pic mémoire réel d'un process reste ce budget PLUS le niveau courant
 * s'il est résident (il est lu pendant toute la transition) : la capacité du
 * tampon est calculée en retranchant ce niveau, jamais en l'ignorant.
 *
 * @param mo Budget en Mo.
 */
void border_ring_dp_set_max_ram_mo(long mo);

/**
 * @brief Reconstruit et délivre, via `on_found`, chaque anneau de bordure
 * réel (pièces réelles, pas classes abstraites) — jusqu'à `max_rings`.
 *
 * `border_ring_count_dp` ne conserve rien d'exploitable une fois le total
 * calculé (chaque niveau de la DP est jeté dès le suivant construit). Cette
 * fonction calcule une SECONDE DP, miroir de la première (classes avec
 * couleur d'entrée/sortie échangées, positions parcourues en sens inverse),
 * dont chaque niveau est cette fois PERSISTÉ sur disque — donnant, pour tout
 * état, le nombre de façons de compléter l'anneau à partir de là. Cette table
 * sert d'oracle d'élagage à un DFS guidé sur l'alphabet des CLASSES
 * (~15-18 sur le jeu réel, jamais les pièces réelles individuellement) :
 * chaque suite de classes complète trouvée est ensuite développée en TOUTES
 * les assignations de pièces réelles possibles (une par combinaison de
 * pièces disponibles quand une classe utilisée a plusieurs pièces encore
 * libres) — cf. `tests/tools/border_ring_dp.c` pour le détail complet et
 * `docs/tests_et_ci.md` pour l'exemple concret (32 anneaux réels sur le jeu
 * 256 pièces, regroupés en un nombre potentiellement inférieur de suites de
 * classes distinctes).
 *
 * Coût disque réel important : la passe arrière persiste TOUS les niveaux
 * intermédiaires (jusqu'à `BORDER_RING_LEN - 1`), chacun pouvant peser autant
 * que son équivalent dans la passe avant (dizaines de Go observés sur le jeu
 * réel, cf. docs/tests_et_ci.md) — `border_ring_dp_set_spill_dir` doit
 * pointer vers un disque avec assez d'espace libre. Coût temps : une passe
 * arrière complète PAR pièce-coin d'ouverture réelle (typiquement ×4 sur le
 * jeu réel, pas de raccourci par rotation ici) — de l'ordre de plusieurs fois
 * le temps d'un `border_ring_count_dp` seul.
 *
 * Le total délivré DOIT correspondre exactement à
 * `border_ring_count_dp(map, all_rotate_parts, nb_workers)` — échec bruyant
 * (`exit(1)`) si ce n'est pas le cas ET que `max_rings` n'a pas coupé la
 * délivrance avant terme (dans ce dernier cas, un total inférieur au total
 * réel est attendu, pas une erreur).
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`).
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param nb_workers       Nombre de process forkés pour la passe arrière
 *                         (même paramètre que `border_ring_count_dp`).
 * @param max_rings        Plafond de sécurité — obligatoire côté appelant
 *                         (`border_mass --max-rings`, pas de valeur par
 *                         défaut) : arrête la délivrance dès qu'il est
 *                         atteint, utile si un jeu de pièces différent
 *                         produisait un total bien plus grand que prévu.
 * @param on_found         Appelé pour chaque anneau réel reconstruit — le
 *                         paquet reçu est un `possibility_packet` complet
 *                         (bordure remplie, intérieur à `-2`), directement
 *                         écrivable dans un fichier `.back` (même format que
 *                         `gen_root`).
 * @param ctx              Passé tel quel à `on_found`.
 * @return                 Nombre total d'anneaux réels délivrés.
 */
long long border_ring_reconstruct_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers,
                                      long long max_rings, border_ring_found_cb on_found, void *ctx);

#endif /* eternityII_border_ring_dp_h */
