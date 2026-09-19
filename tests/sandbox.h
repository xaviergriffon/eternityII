/*
 * Bac à sable du runner de tests unitaires.
 *
 * POURQUOI
 * --------
 * Les modules de production écrivent leurs artefacts sous des chemins RELATIFS
 * ("./eternityII.back", "./eternityII-best_board.back", "events.log", les
 * sockets "etii_main.<pid>"…) : c'est CORRECT en production — un serveur
 * sauvegarde dans SON répertoire de travail — mais mortel sous `make test`, dont
 * le répertoire courant est la racine du dépôt. Un stock de production de 1,96 Go
 * posé à la racine a ainsi été écrasé par un `make test` (en-tête ETERN_SIZE=4 :
 * le fichier venait du build 4×4 de la suite), simplement parce qu'un test
 * exerçait la branche « solution trouvée » de remove_possibilities_with_no_next,
 * laquelle appelle consistent_backup("./eternityII.back", …).
 *
 * LE CHOIX : une correction CENTRALE plutôt que par site.
 * Le runner se déplace une fois pour toutes dans un répertoire temporaire : tout
 * chemin relatif écrit par un test — présent OU FUTUR — y atterrit. La variante
 * « chaque test qui écrit se protège lui-même » a été écartée : elle ne protège
 * pas le prochain test écrit, et c'est précisément par un test nouvellement
 * ajouté que l'accident est arrivé. Les scripts de tests d'intégration
 * (tests/integration/run_*.sh) appliquent déjà exactement ce schéma — résolution des
 * chemins en absolu, puis `cd` dans un `mktemp -d` — ce fichier ne fait que le
 * porter au runner unitaire.
 *
 * CONTREPARTIE, ET SON TRAITEMENT
 * -------------------------------
 * Un test qui LIT une donnée du dépôt par chemin relatif casserait après le
 * `chdir`. Inventaire fait : les seuls chemins de ce genre sont les deux globales
 * de production `parts_files` et `indices_file` (core_static_variables.c, valeurs
 * par défaut "./data/pieces*.csv" / "./data/indices.csv"). Elles sont rendues
 * ABSOLUES avant le `chdir`, donc les tests qui les lisent continuent de lire le
 * dépôt. Les chemins d'ÉCRITURE, eux, restent délibérément relatifs : c'est tout
 * l'objet du bac à sable.
 *
 * GARDE-FOU
 * ---------
 * test_sandbox_enter() photographie le répertoire de lancement ; la suite
 * `sandbox_suite` (tests/test_sandbox.c), jouée EN DERNIER, recompare et échoue
 * si la moindre entrée est apparue ou a disparu. Un futur test qui polluerait la
 * racine du dépôt (par chemin absolu, ou en revenant s'y placer) fait donc
 * échouer `make test` en nommant le fichier fautif.
 */
#ifndef TESTS_SANDBOX_H
#define TESTS_SANDBOX_H

#include <stddef.h>

/**
 * @brief Entre dans le bac à sable : à appeler comme TOUTE PREMIÈRE instruction
 *        du main du runner, avant l'exécution de la moindre suite.
 *
 * Rend absolus les chemins de LECTURE du dépôt (`parts_files`, `indices_file`),
 * photographie le répertoire de lancement, crée un répertoire temporaire et s'y
 * place. Le répertoire est effacé à la sortie du process (atexit), sauf si
 * test_sandbox_keep() a été appelé.
 *
 * En cas d'échec (getcwd, mkdtemp ou chdir), le process s'arrête : tourner sans
 * bac à sable, c'est risquer d'écraser les données du développeur — exactement
 * ce que ce mécanisme existe pour empêcher.
 */
void test_sandbox_enter(void);

/** @brief Répertoire de LANCEMENT (absolu) — la racine du dépôt sous `make test`. */
const char *test_sandbox_origin(void);

/** @brief Répertoire temporaire de travail (absolu), ou "" si non entré. */
const char *test_sandbox_dir(void);

/**
 * @brief Conserve le bac à sable au lieu de l'effacer (post-mortem d'un échec).
 *        Son chemin est alors affiché sur stderr à la sortie.
 */
void test_sandbox_keep(void);

/**
 * @brief Compare le répertoire de lancement à sa photographie d'entrée.
 *
 * @param out    tampon où décrire les écarts ("+ nom" apparu, "- nom" disparu).
 * @param outsz  taille du tampon.
 * @return le nombre d'écarts (0 = répertoire intact), ou -1 si la comparaison
 *         elle-même a échoué (répertoire illisible, bac à sable non entré).
 *
 * Volontairement NON récursif : la pollution observée est toujours à la racine
 * (les défauts de production sont des "./…"), et une descente complète
 * signalerait à tort les .gcda que `make coverage` sème dans les sous-dossiers
 * de couverture.
 */
int test_sandbox_origin_diff(char *out, size_t outsz);

#endif /* TESTS_SANDBOX_H */
