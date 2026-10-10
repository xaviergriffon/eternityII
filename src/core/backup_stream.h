#ifndef eternityII_backup_stream_h
#define eternityII_backup_stream_h

#include <stdint.h>
#include <stdio.h>

/**
 * @file backup_stream.h
 * @brief Ouverture des fichiers de stock (`.back`) : en-tête en clair, puis un
 *        corps éventuellement compressé par zstd — sous `make ZSTD=1`.
 *
 * Le corps d'un `.back` est la suite des enregistrements compacts
 * (`core/packet_codec.h`). Sous `make ZSTD=1`, il est écrit comme UN flux zstd
 * (niveau `BACKUP_STREAM_ZSTD_LEVEL`, somme de contrôle de trame active) et
 * l'en-tête porte la version `PACKET_CODEC_FILE_VERSION_ZSTD`. Mesuré sur un
 * `.back` de production de 3,30 Go en forme compacte : 1,24 Go à zstd -1
 * (×2,66), relu en 4 s.
 *
 * Le flux est un `FILE *` ordinaire (cookie stdio : `fopencookie` sous glibc,
 * `funopen` sous BSD/macOS) : les écrivains (`packet_codec_fwrite`, recopie de
 * l'étage RAM et du débordement) et les lecteurs (`packet_codec_fread`,
 * lecture par morceaux de l'import direct) ne savent pas qu'il est compressé.
 * Seules différences : il ne se positionne pas (`fseek`/`rewind` échouent,
 * rouvrir le fichier), et `fileno` n'y vaut rien — le cache de pages du fichier
 * lu est rendu au noyau par le flux lui-même.
 *
 * Fermer le flux (`fclose`) ferme le fichier sous-jacent ; une erreur d'écriture
 * ou de compression, y compris celle de la fin de trame, fait échouer cet
 * `fclose` — l'appelant n'a pas d'autre contrôle à faire que celui qu'il fait
 * déjà sur un fichier ordinaire.
 *
 * Un binaire compilé SANS zstd lit toujours les `.back` en clair, et refuse
 * bruyamment un `.back` compressé (jamais réinterprété).
 */

/// Niveau de compression des `.back` : le plus rapide, l'écriture se faisant
/// pour l'essentiel sous le gel des pools (zstd -3 gagne encore ×1,3 sur la
/// taille mais coûte ×1,75 de temps de compression).
#define BACKUP_STREAM_ZSTD_LEVEL 1

/// Fils zstd d'une sauvegarde SERVEUR : avec deux, une sauvegarde compressée
/// coûte le temps d'une sauvegarde en clair (2,54 s contre 2,55 s sur 3,4 M
/// possibilités ; 3,16 s sans fil), quatre n'apportent rien de plus.
#define BACKUP_STREAM_SERVER_WORKERS 2

/**
 * @brief Nombre de fils zstd des flux d'écriture ouverts ensuite — 0 par défaut :
 *        compression dans le fil qui écrit.
 *
 * Un processus qui `fork()` ne doit en avoir AUCUN (invariant « aucun thread
 * parent pendant un `fork()` », cf. `app/fork_gate.h`) : un client peut écrire
 * un `.back` (console `backup`, sortie d'urgence) pendant que l'orchestrateur
 * fork ses fils. Seul le serveur, qui ne fork jamais, en demande (`main()`).
 * Sans support multi-fil dans la libzstd, ignoré. Sans zstd, sans effet.
 */
void backup_stream_set_workers(int workers);

/// 1 si ce binaire écrit des `.back` compressés (`make ZSTD=1`), 0 sinon.
int backup_stream_compresses(void);

/// Tests : force l'écriture en clair (0) ou compressée (1, ignoré sans zstd),
/// -1 rétablit le défaut du binaire.
void backup_stream_set_compress_for_tests(int on);

/**
 * @brief Écrit l'en-tête (avec `flags`, `PACKET_CODEC_FILE_FLAG_*`) dans `raw`,
 *        fichier fraîchement ouvert en écriture, et renvoie le flux où écrire
 *        les enregistrements.
 * @return Le flux — `raw` lui-même sans compression —, ou NULL : `raw` est
 *         alors FERMÉ (errno renseigné).
 */
FILE *backup_stream_open_write(FILE *raw, uint8_t flags);

/// Résultat de `backup_stream_open_read`.
enum {
	BACKUP_STREAM_LEGACY = 0,  ///< format hérité : `struct possibility_packet` bruts, sans en-tête
	BACKUP_STREAM_PACKED = 1,  ///< enregistrements compacts (corps en clair ou zstd)
};

/**
 * @brief Détecte le format de `raw` (ouvert en lecture, curseur au début) SUR
 *        LA MAGIE, jamais sur la taille, et renvoie un flux positionné sur le
 *        premier enregistrement.
 *
 * @param name        Nom du fichier, pour le journal.
 * @param out_format  Reçoit `BACKUP_STREAM_LEGACY` ou `BACKUP_STREAM_PACKED`.
 * @param out_header  Si non NULL, reçoit l'en-tête (`PACKET_CODEC_FILE_HEADER_BYTES`
 *                    octets ; mis à zéro pour un fichier hérité).
 * @return Le flux (`raw` lui-même si le corps est en clair), ou NULL — `raw`
 *         est alors FERMÉ et la raison journalisée : en-tête d'une version ou
 *         d'une géométrie incompatible, corps compressé relu par un binaire
 *         sans zstd. Jamais réinterprété.
 */
FILE *backup_stream_open_read(FILE *raw, const char *name, int *out_format, uint8_t *out_header);

/// `fopen(path, "rb")` puis `backup_stream_open_read` — NULL aussi si le
/// fichier ne s'ouvre pas (errno renseigné, rien journalisé).
FILE *backup_stream_fopen_read(const char *path, int *out_format, uint8_t *out_header);

#endif /* eternityII_backup_stream_h */
