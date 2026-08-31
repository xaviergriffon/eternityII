/**
 * @brief Identité déclarée d'un client : nonce machine persistant, nonce de
 *        session, rang de fork, libellé — codec pur (sans I/O réseau)
 *        partagé par les deux hellos du protocole :
 *         - le hello de la connexion de travail (`INST_CLIENT_HELLO`),
 *           envoyé par chaque fork après le handshake ;
 *         - le hello du canal de contrôle (`control_hello_t`, étendu de ces
 *           mêmes champs), envoyé par le process parent.
 *
 * Module séparé plutôt que dupliquer l'encodage dans les deux protocoles :
 * les deux hellos transportent exactement les mêmes champs, dont un champ
 * de longueur variable (`label`) qu'il faut borner et cadrer correctement
 * des deux côtés.
 *
 * Quatre notions distinctes, à ne jamais fusionner : `machine_uid`
 * (persistant, survit aux redémarrages — clé de cumul), `client_uid`
 * (nonce d'une exécution du process parent — identité de session,
 * propriétaire des baux), `fork_seq` (rang d'un fork — rattache une
 * connexion de travail à son parent) et `label` (déclaratif, affichage
 * seul, jamais une clé — deux clients peuvent légitimement le partager).
 */
#ifndef eternityII_client_identity_h
#define eternityII_client_identity_h

#include <stddef.h>
#include <stdint.h>

/// Taille en octets du nonce machine (persistant, cf. `machine_uid_load_or_create`).
#define MACHINE_UID_BYTES 16
/// Taille en octets du nonce de session (tiré à chaque démarrage de process parent).
#define CLIENT_UID_BYTES 16
/// Taille maximale (terminateur NUL inclus) du libellé déclaré (option CLI
/// `--name`, ou nom d'hôte par défaut). Bornée pour rester un affichage
/// console/HTTP court — jamais une donnée arbitrairement longue sur le fil.
#define CLIENT_LABEL_MAX 32

/// Mode « recherche » (valeur historique de `control_hello_t.mode` avant v12).
#define CLIENT_MODE_SEARCH 0
/// Mode « pruner CPU ».
#define CLIENT_MODE_PRUNER 1
/// Mode « pruner GPU » (`--gpu`, build CUDA uniquement).
#define CLIENT_MODE_GPU_PRUNER 2

/**
 * @brief Identité déclarée d'UNE connexion (de travail ou de contrôle).
 *
 * Déclarative par nature : rien ici n'est vérifié par le serveur, à la
 * différence de `client_t.peer_ip` (dérivé de `accept()`). `fork_seq`
 * distingue les deux hellos : -1 pour le
 * canal de contrôle (le process PARENT, pas un fork particulier), 0..N-1
 * pour la connexion de travail d'un fork donné.
 */
typedef struct {
    /// Nonce machine persistant (identique pour tous les process d'une même
    /// machine, survit aux redémarrages — cf. `machine_uid_load_or_create`).
    uint8_t machine_uid[MACHINE_UID_BYTES];
    /// Nonce de session, tiré une fois par démarrage de process PARENT et
    /// partagé (hérité par `fork()`) par tous ses forks.
    uint8_t client_uid[CLIENT_UID_BYTES];
    /// -1 = canal de contrôle (process parent) ; 0..N-1 = rang du fork sur sa
    /// propre connexion de travail.
    int32_t fork_seq;
    /// Mode du client : cf. `CLIENT_MODE_*`.
    uint8_t mode;
    /// Libellé déclaré, toujours NUL-terminé (option CLI `--name`, ou nom
    /// d'hôte par défaut). Jamais une clé — uniquement pour l'affichage.
    char label[CLIENT_LABEL_MAX];
} client_identity_t;

/// Taille minimale sur le fil (label vide) : machine_uid + client_uid +
/// fork_seq (int32) + mode (uint8) + longueur de label préfixée (uint8).
#define CLIENT_IDENTITY_WIRE_MIN_SIZE (MACHINE_UID_BYTES + CLIENT_UID_BYTES + 4 + 1 + 1)
/// Taille maximale sur le fil (label à sa longueur maximale) : à utiliser
/// pour dimensionner tout tampon d'émission/réception de cette structure.
#define CLIENT_IDENTITY_WIRE_MAX_SIZE (CLIENT_IDENTITY_WIRE_MIN_SIZE + (CLIENT_LABEL_MAX - 1))

/**
 * @brief Sérialise `id` dans `buf` (champ par champ, largeur fixe pour tout
 *        sauf `label`, préfixé par sa longueur réelle sur 1 octet).
 *
 * @param id      Structure source.
 * @param buf     Tampon destination, au moins `CLIENT_IDENTITY_WIRE_MAX_SIZE`
 *                octets recommandés (ou `bufsize` suffisant pour ce label précis).
 * @param bufsize Taille de `buf`.
 * @return        Le nombre d'octets écrits (`CLIENT_IDENTITY_WIRE_MIN_SIZE` +
 *                longueur réelle du label), ou -1 si `buf` est trop petit.
 */
int32_t client_identity_encode(const client_identity_t *id, uint8_t *buf, size_t bufsize);

/**
 * @brief Désérialise `client_identity_t` depuis `buf`.
 *
 * Rejette (retourne -1, sans jamais lire au-delà de `len`) : un `len` trop
 * court pour les champs de largeur fixe, une longueur de label déclarée
 * dépassant `CLIENT_LABEL_MAX - 1`, ou dépassant les octets réellement
 * disponibles dans `buf`.
 *
 * @param buf Tampon source.
 * @param len Nombre d'octets disponibles dans `buf`.
 * @param out Structure destination (`label` toujours NUL-terminé en sortie).
 * @return    0 si OK, -1 sinon.
 */
int client_identity_decode(const uint8_t *buf, int32_t len, client_identity_t *out);

/**
 * @brief Encode `n` octets en hexadécimal minuscule dans `out`.
 *
 * @param bytes    Tampon source.
 * @param n        Nombre d'octets à encoder.
 * @param out      Tampon destination, terminé par NUL en cas de succès.
 * @param out_size Taille de `out` (doit être au moins `2*n + 1`).
 * @return         Longueur écrite hors NUL (`2*n`), ou -1 si `out` trop petit
 *                 ou `bytes`/`out` est `NULL` alors que `n > 0`.
 */
int client_identity_hex_encode(const uint8_t *bytes, size_t n, char *out, size_t out_size);

/**
 * @brief Décode une chaîne hexadécimale (insensible à la casse, EXACTEMENT
 *        `2*n` caractères, sans séparateur) vers `out`.
 *
 * @param hex Chaîne source NUL-terminée.
 * @param out Tampon destination, au moins `n` octets.
 * @param n   Nombre d'octets attendus en sortie.
 * @return    0 si OK, -1 si `hex` n'a pas EXACTEMENT `2*n` caractères
 *            hexadécimaux valides (ni plus court, ni plus long).
 */
int client_identity_hex_decode(const char *hex, uint8_t *out, size_t n);

/**
 * @brief Tire `n` octets aléatoires cryptographiquement sûrs (`getentropy(2)`,
 *        disponible sans dépendance supplémentaire sur macOS et Linux/glibc
 *        récents). `n` est plafonné à 256 octets par `getentropy`, très
 *        au-delà des besoins de ce module (16 octets).
 *
 * @param out Tampon destination.
 * @param n   Nombre d'octets à tirer.
 * @return    0 si OK, -1 en cas d'échec (ex. plateforme sans `getentropy`) —
 *            l'appelant doit alors dégrader explicitement, jamais utiliser
 *            silencieusement un tampon partiellement rempli.
 */
int client_identity_random_bytes(uint8_t *out, size_t n);

/**
 * @brief Résultat de `machine_uid_load_or_create`.
 */
typedef enum {
    /// Fichier existant et valide, chargé tel quel.
    MACHINE_UID_LOADED = 0,
    /// Fichier absent ou invalide : un nouveau nonce a été tiré ET écrit avec succès.
    MACHINE_UID_CREATED = 1,
    /// Un nouveau nonce a été tiré mais n'a PAS pu être écrit (répertoire non
    /// inscriptible, etc.) : `out` reste rempli, mais l'identité ne survivra
    /// pas à ce process — l'impossibilité de cumuler ne doit jamais empêcher
    /// de chercher.
    MACHINE_UID_VOLATILE = 2
} machine_uid_status_t;

/**
 * @brief Charge le nonce machine persistant depuis `path` (contenu attendu :
 *        `2*MACHINE_UID_BYTES` caractères hexadécimaux, espaces/retour à la
 *        ligne de fin tolérés), ou en tire un nouveau et tente de l'écrire si
 *        le fichier est absent, illisible ou de contenu invalide.
 *
 * Ne fait JAMAIS échouer le process appelant : dans tous les cas `out` est
 * rempli avec un nonce utilisable (chargé ou fraîchement tiré).
 *
 * @param path Chemin du fichier d'identité machine.
 * @param out  Tampon destination, `MACHINE_UID_BYTES` octets.
 * @return     Le statut de l'opération (cf. `machine_uid_status_t`).
 */
machine_uid_status_t machine_uid_load_or_create(const char *path, uint8_t out[MACHINE_UID_BYTES]);

#endif /* eternityII_client_identity_h */
