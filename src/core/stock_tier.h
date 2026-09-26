/**
 * @file stock_tier.h
 * @brief Étage RAM du stock en BLOCS : les possibilités froides d'une file,
 *        rangées côte à côte sous forme compacte plutôt qu'en maillons de
 *        liste chaînée.
 *
 * Un maillon `Element` (`core/lifo.h`) coûte 112 octets à l'allocateur pour
 * 70 octets de forme compacte : 38 % de la RAM du stock sert au chaînage.
 * Un bloc range jusqu'à `STOCK_TIER_BLOCK_BYTES` d'enregistrements compacts
 * (`core/packet_codec.h`) bout à bout, pour un seul chunk malloc — ×1,60
 * mesuré sur le stock de production. Compilé avec `make ZSTD=1`, chaque bloc
 * est de plus compressé par zstd -1 : ×3,57 au total, 31 octets par
 * possibilité (docs/conception/etage_ram_compresse.md). La compression est
 * entièrement cachée derrière `stock_tier_push`/`stock_tier_block_unpack` :
 * aucun appelant ne voit la forme stockée.
 *
 * Ce module est une STRUCTURE DE DONNÉES : une pile de blocs par (pool, file),
 * sans verrou ni connaissance du `datamanager` ou du disque. L'appelant
 * sérialise l'accès à une pile. Seuls états partagés : le codec choisi à la
 * compilation, et un contexte zstd par thread qui en compresse.
 *
 * Invariants — les rompre perd ou duplique des possibilités :
 *  - **Le bloc est l'unité atomique.** Il entre en entier (`stock_tier_push`)
 *    et sort en entier : `stock_tier_block_unpack` le LIT sans le retirer,
 *    `stock_tier_pop_top`/`_pop_bottom` le retire une fois ses possibilités
 *    confirmées ailleurs (« peek puis commit »). Jamais de bloc entamé : c'est
 *    ce qui permet des enregistrements de TAILLE VARIABLE sans l'arithmétique
 *    à pas variable écartée pour les segments de débordement.
 *  - **Un bloc est validé en entier AVANT d'être accepté ou rendu.** Ses
 *    enregistrements doivent paver exactement ses octets bruts (longueur de
 *    chacun déduite de son bitmap, `packet_codec_peek_placed`). Un contenu qui
 *    ne se pave pas est refusé à l'entrée, et un bloc qui ne se relit pas est
 *    signalé à la sortie SANS avoir rien rendu.
 *    Un bloc compressé porte la somme de contrôle de trame zstd : abîmé en
 *    mémoire, il ne se relit pas, au lieu de se relire faux mais à la bonne
 *    taille.
 *  - **`bytes` est le coût pour l'allocateur**, pas la charge utile : c'est ce
 *    que le plafond RAM (`--stock-max-ram`) doit compter.
 *
 * L'ordre est celui d'une pile : `top` est le bloc le plus récemment empilé
 * (le moins froid), `bottom` le plus ancien. Dans un bloc, les enregistrements
 * gardent l'ordre dans lequel ils ont été fournis.
 */
#ifndef eternityII_stock_tier_h
#define eternityII_stock_tier_h

#include <stddef.h>
#include <stdint.h>

/// Taille maximale des octets BRUTS d'un bloc — 64 Kio, soit ~930
/// possibilités de 70 octets. Mesuré (`make bench-ram-tier`) : 16 Kio coûte
/// en ratio une fois compressé, 256 Kio quadruple l'unité de rechargement pour
/// 5 % de ratio.
#define STOCK_TIER_BLOCK_BYTES (64 * 1024)

/// Surcoût d'allocateur compté par bloc — même tarif que
/// `DATAMANAGER_MALLOC_OVERHEAD` pour un maillon, qui n'est pas visible d'ici.
/// Négligeable devant 64 Kio ; il n'existe que pour ne pas compter un bloc
/// vide comme gratuit.
#define STOCK_TIER_MALLOC_OVERHEAD 16

/// Forme des octets stockés d'un bloc : brute, ou compressée par zstd
/// (`make ZSTD=1`). Un bloc que zstd n'arrive pas à réduire reste brut.
#define STOCK_TIER_CODEC_RAW 0
#define STOCK_TIER_CODEC_ZSTD 1

/// Niveau zstd : -1 mesuré à ×2,23 sur la forme compacte en blocs de 64 Kio,
/// 2,7 M possibilités/s en éviction ; -3 gagne 6 % de ratio pour un tiers de
/// débit en moins (`make bench-ram-tier`).
#define STOCK_TIER_ZSTD_LEVEL 1

/// Compression des blocs empilés désormais : « zstd -1 » ou « aucune ».
const char *stock_tier_compression(void);

typedef struct stock_tier_block stock_tier_block_t;

/**
 * @brief Pile de blocs d'une (pool, file). Initialiser par
 *        `stock_tier_stack_init`, jamais à la main.
 *
 * Les compteurs sont tenus par le module et lisibles directement.
 */
typedef struct {
	stock_tier_block_t *bottom;
	stock_tier_block_t *top;
	/// Possibilités détenues, tous blocs confondus.
	unsigned long long records;
	/// Octets tenus auprès de l'allocateur (en-têtes et surcoût compris).
	unsigned long long bytes;
	/// Nombre de blocs.
	unsigned long long blocks;
	/// Numéro du dernier bloc empilé (0 : aucun encore). Croissant du bas vers
	/// le haut, jamais réutilisé tant que la pile n'est pas réinitialisée : une
	/// passe d'expansion en fait sa frontière (`stock_tier_block_seq`).
	unsigned long long last_seq;
} stock_tier_stack_t;

void stock_tier_stack_init(stock_tier_stack_t *stack);

/// Libère tous les blocs et remet la pile à vide. Ce qu'elle contenait est
/// PERDU : réservé à un appelant qui en a déjà une autre copie, ou qui
/// l'abandonne délibérément (arrêt, restauration qui remplace le stock).
void stock_tier_stack_clear(stock_tier_stack_t *stack);

/**
 * @brief Longueur de l'enregistrement compact qui commence à `rec`.
 * @param avail Octets lisibles depuis `rec`.
 * @return La longueur, ou 0 si l'enregistrement dépasse `avail` (ou si même
 *         son en-tête et son bitmap n'y tiennent pas).
 */
size_t stock_tier_record_len(const uint8_t *rec, size_t avail);

/**
 * @brief Nombre d'enregistrements compacts qui pavent EXACTEMENT `raw`.
 * @return Le nombre (> 0), ou -1 si `raw` est vide, dépasse
 *         `STOCK_TIER_BLOCK_BYTES` ou ne se découpe pas en enregistrements
 *         entiers.
 */
int stock_tier_count_records(const uint8_t *raw, size_t raw_bytes);

/**
 * @brief Empile un bloc au SOMMET, fait des `raw_bytes` octets de `raw`
 *        (enregistrements compacts concaténés, recopiés).
 *
 * @return Le nombre de possibilités empilées, ou -1 sans rien modifier si
 *         `raw` est invalide (cf. `stock_tier_count_records`) ou si
 *         l'allocation échoue. Sur -1, `raw` appartient toujours à
 *         l'appelant, qui doit le remettre ailleurs.
 */
int stock_tier_push(stock_tier_stack_t *stack, const uint8_t *raw, size_t raw_bytes);

/// Bloc du sommet (le plus récent), NULL si la pile est vide.
const stock_tier_block_t *stock_tier_top(const stock_tier_stack_t *stack);

/// Bloc du bas (le plus ancien), NULL si la pile est vide.
const stock_tier_block_t *stock_tier_bottom(const stock_tier_stack_t *stack);

/// Bloc immédiatement au-dessus de `block` (plus récent), NULL au sommet.
/// Parcours du bas vers le haut, sans rien retirer — pour une sauvegarde.
const stock_tier_block_t *stock_tier_block_above(const stock_tier_block_t *block);

/// Numéro d'empilement de `block` : `stack->last_seq` au moment du push.
/// Un bloc de numéro <= N était déjà dans la pile quand `last_seq` valait N.
unsigned long long stock_tier_block_seq(const stock_tier_block_t *block);

/// `STOCK_TIER_CODEC_RAW` ou `STOCK_TIER_CODEC_ZSTD`.
int stock_tier_block_codec(const stock_tier_block_t *block);

/// Octets réellement stockés (compressés s'il y a lieu) — ce qui est compté.
size_t stock_tier_block_stored_bytes(const stock_tier_block_t *block);

uint32_t stock_tier_block_records(const stock_tier_block_t *block);
size_t stock_tier_block_raw_bytes(const stock_tier_block_t *block);

/**
 * @brief Recopie les octets bruts de `block` dans `out`, SANS le retirer.
 *
 * Le contenu est revalidé (même pavage qu'à l'entrée, même nombre
 * d'enregistrements) avant d'être rendu : sur échec, `out` n'est pas
 * utilisable et rien n'a été rendu.
 *
 * @param cap Octets disponibles dans `out` — `STOCK_TIER_BLOCK_BYTES` suffit
 *            toujours.
 * @return Le nombre d'enregistrements, ou -1 (`cap` trop petit, bloc
 *         incohérent).
 */
int stock_tier_block_unpack(const stock_tier_block_t *block, uint8_t *out, size_t cap);

/// Retire et libère le bloc du sommet (commit après un rechargement). No-op
/// sur une pile vide.
void stock_tier_pop_top(stock_tier_stack_t *stack);

/// Retire et libère le bloc du bas (commit après un transfert vers le disque
/// ou une lecture par une passe d'expansion). No-op sur une pile vide.
void stock_tier_pop_bottom(stock_tier_stack_t *stack);

/// Retire et libère `block`, où qu'il soit dans `stack` — pendant une passe
/// d'expansion, le transfert vers le disque prend le plus ancien bloc écrit
/// DEPUIS le début de la passe, au-dessus de ceux qu'elle n'a pas encore lus.
/// `block` doit appartenir à `stack`.
void stock_tier_remove(stock_tier_stack_t *stack, const stock_tier_block_t *block);

#endif
