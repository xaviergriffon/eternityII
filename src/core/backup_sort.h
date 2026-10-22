/**
 * @file backup_sort.h
 * @brief Remise en ordre d'un `.back` : tri externe de ses enregistrements dans
 *        l'ordre de l'arbre (mode `eternityII sortback`).
 *
 * Sous `make ZSTD=1`, la place d'une possibilité dépend surtout de l'ORDRE du
 * stock : des blocs de frères et cousins tiennent 4,5 octets par possibilité au
 * niveau 26, les mêmes possibilités dans le désordre 12,5 à 37 (mesuré,
 * docs/utilisation.md « L'ordre du stock fait sa compacité »). Trier les
 * enregistrements compacts par plateau (bitmap des cases posées, puis leurs
 * valeurs) remet côte à côte ce qui partage le plus de cases : un échantillon
 * de production mélangé (37,2 octets) revient à 4,3, mieux que son ordre
 * d'origine (4,6). Le tri doit être GLOBAL : par fenêtres de 1 M sur 6 M, il
 * ne rend que 8,1 octets, car les parents d'une famille sont dispersés dans
 * tout le stock. D'où un tri externe : des paquets triés en RAM sous un budget,
 * écrits sur disque, puis fusionnés.
 *
 * Parallélisme : la lecture de l'entrée remplit un tampon pendant que l'autre
 * est trié sur plusieurs fils (une tranche chacun, puis fusion des tranches)
 * et écrit ; les flux zstd des paquets et de la sortie compressent sur les
 * fils de `backup_stream_set_workers`. Le résultat ne dépend pas du nombre de
 * fils : deux enregistrements égaux pour la clé sont identiques à l'octet.
 *
 * Contrat :
 *  - la sortie porte EXACTEMENT le multi-ensemble d'enregistrements que
 *    `restore` tirerait de l'entrée — mis sous la forme canonique
 *    (`packet_codec_canonicalize`, score MRV « inconnu ») que la restauration
 *    leur donne de toute façon ; seul l'ordre change ;
 *  - l'en-tête de la sortie reprend les drapeaux de l'entrée (un `.back`
 *    autonome le reste) ;
 *  - un `.back` hérité (`struct possibility_packet` bruts) est accepté et
 *    sorti compacté, normalisé comme `import` le normalise ;
 *  - un `.back` NON autonome accompagné de son `.spillcount` est refusé : le
 *    débordement qu'il nomme resterait hors du tri, et la sortie ne serait
 *    plus restaurable avec lui. Faire d'abord une sauvegarde `backup` ;
 *  - la sortie est écrite sous un nom temporaire puis renommée : un échec ne
 *    laisse ni sortie partielle ni fichier de travail. Une sortie existante
 *    n'est jamais écrasée.
 */
#ifndef eternityII_backup_sort_h
#define eternityII_backup_sort_h

#include <stddef.h>
#include <stdint.h>

/// Budget RAM par défaut (Mo) : deux tampons de la moitié chacun, l'un lu
/// pendant que l'autre est trié et écrit.
#define BACKUP_SORT_MEM_MB_DEFAULT 4096
/// Fils de tri d'un paquet : au plus.
#define BACKUP_SORT_THREADS_MAX 64
/// Fils de tri par défaut : un par cœur en ligne, au plus.
#define BACKUP_SORT_THREADS_AUTO_MAX 16

typedef struct {
	size_t mem_bytes;        ///< Budget RAM des deux tampons (enregistrements + index).
	const char *tmp_dir;     ///< Où écrire les paquets ; NULL : dossier de la sortie.
	int threads;             ///< Fils de tri ; 0 : `backup_sort_auto_threads`.
} backup_sort_options_t;

/// Un fil par cœur en ligne, au plus `BACKUP_SORT_THREADS_AUTO_MAX`.
int backup_sort_auto_threads(void);

typedef struct {
	unsigned long long records;     ///< Enregistrements lus (= écrits).
	unsigned long long runs;        ///< Paquets triés (1 : tout a tenu en RAM, pas de fusion).
	unsigned long long duplicates;  ///< Enregistrements identiques à leur prédécesseur (gardés).
	unsigned long long raw_bytes;   ///< Octets compacts écrits (avant compression du flux).
} backup_sort_stats_t;

enum {
	BACKUP_SORT_OK = 0,
	BACKUP_SORT_ERR_INPUT = -1,       ///< Entrée illisible, d'une autre géométrie, ou tronquée.
	BACKUP_SORT_ERR_OUTPUT_EXISTS = -2,
	BACKUP_SORT_ERR_INCOMPLETE = -3,  ///< Entrée non autonome avec un `.spillcount`.
	BACKUP_SORT_ERR_IO = -4,          ///< Écriture d'un paquet ou de la sortie, mémoire.
};

/**
 * @brief Ordre de l'arbre entre deux enregistrements compacts : bitmap des
 *        cases posées, puis valeurs, puis en-tête ; à préfixe égal, le plus
 *        court d'abord. Pure ; 0 si et seulement si les octets sont identiques.
 */
int backup_sort_record_cmp(const uint8_t *a, size_t la, const uint8_t *b, size_t lb);

/**
 * @brief Trie `in` dans `out` (cf. contrat ci-dessus).
 * @param stats Rempli si non NULL (y compris en cas d'échec, pour le journal).
 * @return `BACKUP_SORT_OK` ou une erreur `BACKUP_SORT_ERR_*` (raison journalisée).
 */
int backup_sort(const char *in, const char *out, const backup_sort_options_t *opts, backup_sort_stats_t *stats);

#endif /* eternityII_backup_sort_h */
