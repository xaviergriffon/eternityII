/**
 * @file stock_rate.h
 * @brief Débit d'ajouts/consommations du stock de possibilités, moyenné sur
 *        trois fenêtres glissantes (1 minute, 1 heure, 1 jour).
 *
 * Module côté serveur uniquement : instrumente `put_to_pool`/`scroll_from_pool`
 * (`core/datamanager.c`) pour répondre à « à quelle vitesse le stock
 * grossit/se vide en ce moment ? », sans attendre qu'un niveau instantané
 * change visiblement. Ne dépend d'aucun module `app/` (règle AGENTS.md).
 */
#ifndef eternityII_stock_rate_h
#define eternityII_stock_rate_h

#include <time.h>
#include <stdint.h>

/// Granularité fine (1s) : couvre la fenêtre 1 minute.
#define STOCK_RATE_SEC_BUCKETS 60
/// Granularité grossière (1min) : couvre les fenêtres 1 heure (60 derniers
/// buckets) et 1 jour (les 1440 buckets).
#define STOCK_RATE_MIN_BUCKETS 1440

/**
 * @brief Un bucket = un compteur d'événements pour une seconde ou une minute
 *        donnée, identifiée par `epoch` (le quotient horodatage/granularité).
 *
 * `epoch` sert à détecter un bucket périmé (tour précédent du ring buffer) :
 * `stock_rate_record` le remet à zéro avant d'y écrire si `epoch` ne
 * correspond plus à l'horodatage courant. Champs accédés via `__atomic_*`
 * (même convention que le reste du projet — `possibility.c`, `etii_search.c`,
 * `app_runtime.c` — jamais le mot-clé `_Atomic` ni `<stdatomic.h>`), sans
 * verrou : cohérent avec la philosophie PR1 (jamais de blocage sur le chemin
 * chaud `put_to_pool`/`scroll_from_pool`) ; une statistique d'affichage
 * tolère la rare course entre la remise à zéro et l'incrément suivant.
 */
typedef struct {
    uint64_t epoch;
    uint32_t count;
} rate_bucket_t;

/**
 * @brief Compteur d'événements (ajouts OU consommations, un par direction)
 *        sur les deux granularités nécessaires aux trois fenêtres.
 */
typedef struct {
    rate_bucket_t sec[STOCK_RATE_SEC_BUCKETS];
    rate_bucket_t min[STOCK_RATE_MIN_BUCKETS];
} stock_rate_counter_t;

/**
 * @brief Enregistre `n` événements survenus à l'instant `now`.
 *
 * `now` est toujours fourni par l'appelant (jamais `time(NULL)` en interne) :
 * les sites de production passent l'horodatage réel, les tests des valeurs
 * synthétiques croissantes — c'est ce qui rend les fenêtres heure/jour
 * testables sans attendre ni mocker l'horloge système.
 *
 * @param c   Compteur à mettre à jour (`&stock_adds_rate`/`&stock_removes_rate`).
 * @param n   Nombre d'événements à ajouter (0 : no-op).
 * @param now Horodatage de référence de ces événements.
 */
void stock_rate_record(stock_rate_counter_t *c, unsigned int n, time_t now);

/**
 * @brief Calcule le débit moyen (événements/seconde) sur les trois fenêtres,
 *        à l'instant `now`.
 *
 * Une fenêtre dont la durée réelle observée est plus courte que sa taille
 * nominale (ex. serveur démarré il y a 10 minutes : la fenêtre "jour" ne
 * couvre en réalité que ces 10 minutes) n'est PAS distinguée ici — le
 * dénominateur reste la taille nominale de la fenêtre (60/3600/86400s), donc
 * le débit affiché est sous-estimé tant que le serveur est jeune. Comportement
 * assumé : une statistique de tendance, pas un compteur exact, et qui
 * converge vers la vraie valeur en quelques fenêtres.
 *
 * @param c           Compteur à interroger.
 * @param now         Horodatage de référence.
 * @param per_sec_1m  Sortie : débit moyen sur la dernière minute.
 * @param per_sec_1h  Sortie : débit moyen sur la dernière heure.
 * @param per_sec_1d  Sortie : débit moyen sur le dernier jour.
 */
void stock_rate_windows(const stock_rate_counter_t *c, time_t now,
                         double *per_sec_1m, double *per_sec_1h, double *per_sec_1d);

/**
 * @brief Remet `c` à un état vierge (tous les buckets à zéro/époque 0).
 *
 * Réservée aux tests, pour l'isolation entre cas (même rôle que
 * `datamanager_reset_rr_state_for_tests`).
 */
void stock_rate_reset_for_tests(stock_rate_counter_t *c);

#endif
