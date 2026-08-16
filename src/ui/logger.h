#ifndef logger_h
#define logger_h

/** @brief Affiche un message d'erreur suivi du message système associé à `errno` (sur stderr). */
void log_errno(const char *format, ...);

/** @brief Affiche un message d'erreur sur stderr. */
void log_error(const char *format, ...);

/**
 * @brief Journalise un message d'erreur fatal via log_error puis termine le
 *        process avec exit(EXIT_FAILURE).
 *
 * Funnel unique des erreurs fatales : centralise le motif répété
 * `log_error(...); exit(EXIT_FAILURE);`. Marqué `noreturn`, donc l'appelant n'a
 * pas à gérer le retour (ni à ajouter un exit() défensif derrière).
 *
 * Conçu comme point de couture pour les tests : les chemins fataux deviennent
 * testables via fork (cf. tests/fork_assert.h) — le fils appelle fatal_error et
 * sort, le parent inspecte WEXITSTATUS — et offrent un point central unique pour
 * une future gestion globale (dump de crash, sortie structurée…).
 */
__attribute__((noreturn, format(printf, 1, 2)))
void fatal_error(const char *format, ...);

/** @brief Affiche un message informatif sur stdout. */
void log_info(const char *format, ...);

/** @brief Affiche un message de débogage sur stdout. */
void log_debug(const char *format, ...);

/** @brief Affiche un message destiné à l'affichage interactif de la console. */
void log_console(const char *format, ...);

/**
 * @brief Enregistre un événement notable (nouveau record, demande non satisfaite,
 *        solution...) dans la zone d'affichage fixe et dans le journal `events.log`.
 *
 * Le message est horodaté, conservé dans un buffer circulaire (les N derniers sont
 * affichés dans la zone fixe en haut du terminal) et ajouté au fichier de log.
 * Si la zone fixe n'est pas active (sortie non interactive), le message est imprimé
 * normalement.
 */
void log_event(const char *format, ...);

/**
 * @brief Journalise un message UNIQUEMENT dans `events.log` — jamais sur la
 *        console, jamais dans le buffer circulaire de la zone d'événements.
 *
 * Contrairement à log_event (borné à 200 octets, dimensionné pour tenir sur
 * une ligne de la zone fixe), accepte jusqu'à `LOG_LINE_MAX` (4096) octets et
 * conserve les sauts de ligne internes : réservé aux diagnostics volumineux
 * (ex. dump de la configuration effective au démarrage) qui noieraient la
 * console ou la zone d'événements s'ils y étaient affichés, mais doivent
 * rester consultables après coup (`tail -f events.log`). Réservé au process
 * PARENT : à la différence des autres fonctions de ce fichier, ne route rien
 * vers le parent si appelé depuis un fils forké.
 */
void log_file(const char *format, ...);

/**
 * @brief Met à jour le bandeau de statistiques « live » (vitesse, stock, record…).
 *
 * En mode ncurses, remplace le contenu d'une ligne d'état fixe rafraîchie en
 * continu par le thread de statistiques. En mode ANSI (sans ncurses), c'est un
 * no-op : les statistiques restent consultables via la commande `check`.
 */
void log_status(const char *format, ...);
/** @brief Vide le tampon de sortie standard (pour `log_console`). */
void flush_console(void);
/** @brief Vide le tampon de sortie standard (pour `log_debug`). */
void flush_debug(void);
/** @brief Vide le tampon d'erreur standard (pour `log_error` / `log_errno`). */
void flush_error(void);
/** @brief Vide le tampon de sortie standard (pour `log_info`). */
void flush_info(void);

/**
 * @brief Prend/relâche le verrou interne qui sérialise les écritures sur le
 *        terminal (`output_mutex`, ANSI comme ncurses).
 *
 * Exposé pour `src/app/fork_gate.c` (infrastructure de quiescence
 * coopérative) : avant un `fork()`, le thread
 * forkeur doit détenir CE verrou en plus de `flockfile(stdout)`/
 * `flockfile(stderr)`, sans quoi un autre thread pourrait être au milieu d'un
 * log au moment du fork et transmettre son état à l'enfant sans le thread qui
 * le détient. Non ré-entrant, comme le `pthread_mutex_t` sous-jacent.
 */
void logger_lock_output(void);
/** @brief Relâche le verrou pris par `logger_lock_output`. */
void logger_unlock_output(void);

/**
 * @brief Efface l'écran de la console interactive sans détruire l'historique.
 *
 * En mode ANSI, le contenu visible est poussé dans le scrollback natif du
 * terminal (molette / Cmd+↑) au lieu d'être effacé. En mode ncurses, la vue
 * devient blanche mais le pad de sortie est conservé (PgUp pour y revenir).
 * Appelée par la commande `clear` et le raccourci Ctrl-L.
 */
void clear_console(void);

/**
 * @brief Publie la ligne de saisie interactive courante (prompt + saisie) et la redessine.
 *
 * Mode ANSI uniquement (no-op en ncurses, qui a sa propre fenêtre de saisie).
 * Tant qu'une saisie est active, chaque écriture de log terminée par un saut de
 * ligne efface la ligne de saisie, écrit le log, puis la redessine en dessous —
 * les logs asynchrones (thread de statistiques, événements relayés des forks)
 * ne corrompent plus la ligne en cours de frappe.
 *
 * @param prompt Invite affichée avant la ligne (ex. "commande :").
 * @param line   Contenu courant de la ligne éditée.
 * @param cursor Position du curseur dans @p line (0..strlen(line)) : le
 *               curseur terminal est repositionné en conséquence après le
 *               redessin, pour permettre l'édition au milieu de la ligne
 *               (←/→, Home/End...).
 */
void console_input_render(const char *prompt, const char *line, int cursor);

/**
 * @brief Termine la saisie interactive (Entrée) : imprime le saut de ligne et
 *        cesse de protéger/redessiner la ligne de saisie.
 */
void console_input_end(void);

/**
 * @brief Active la pagination de la sortie pour la commande console qui démarre.
 *
 * Mode ANSI uniquement (no-op en ncurses, où le pad + PgUp couvrent le besoin).
 * À appeler depuis le thread console juste avant do_command_line : les écritures
 * de CE thread marquent une pause « --Suite-- » chaque fois qu'une page d'écran
 * est remplie (espace : page suivante, entrée : ligne suivante, q : dérouler la
 * suite sans pause — rien n'est jamais supprimé). Les logs des AUTRES threads ne
 * sont ni paginés ni retenus : le verrou d'affichage est relâché pendant
 * l'attente d'une touche, l'affichage asynchrone reste vivant. Sans effet si
 * stdin/stdout ne sont pas des terminaux ou si l'écran est trop petit.
 */
void console_pager_begin(void);

/** @brief Désactive la pagination (fin de la commande console). */
void console_pager_end(void);
/** @brief Installe la zone d'affichage fixe (région de défilement ANSI). À appeler depuis le thread console. */
void status_zone_init(void);
/** @brief Restaure le terminal (région de défilement complète). Enregistré via atexit par status_zone_init. */
void status_zone_teardown(void);

/**
 * @brief Neutralise, dans CE process, l'`atexit(status_zone_teardown)`
 *        hérité d'un `fork()` — à appeler UNE SEULE FOIS, comme tout premier
 *        traitement d'un process de recherche fraîchement forké (avant tout
 *        `log_*`), jamais par le process PARENT.
 *
 * `status_zone_init()` (`console.c`) est appelée UNE FOIS, dans le PARENT,
 * AVANT tout fork (démarrage différé, cf. l'orchestrateur de démarrage
 * différé) — `fork()` duplique donc la liste des
 * handlers `atexit()`, si bien que chaque fils de recherche hérite lui aussi
 * l'enregistrement de `status_zone_teardown`, bien qu'il ne "possède" jamais
 * le terminal partagé. Sans ce garde-fou, le `exit()` normal d'un fils (fin
 * de recherche, OU sortie propre après un `stopForks`/`configApply` — jamais
 * après un SIGKILL, qui saute `atexit` : d'où le caractère intermittent
 * observé) ré-exécute ce handler hérité et restaure le terminal (`endwin()`
 * en `NCURSES=1`, région de défilement complète en ANSI) — visible depuis le
 * PARENT puisque le terminal est un état PARTAGÉ (même descripteur hérité),
 * pas un état par-process.
 *
 * Ne touche JAMAIS le terminal elle-même : elle rend seulement le handler
 * hérité NO-OP dans CE process (écriture dans la copie COW du drapeau
 * `zone_active`/`nc_active` du fils, sans effet sur le parent) —
 * `status_zone_teardown()` a déjà, pour cette même raison, un garde
 * `if (!zone_active) return;` en tête.
 */
void status_zone_disown_child(void);

#ifdef USE_NCURSES
/**
 * @brief Boucle interactive de la console implémentée avec ncurses.
 *
 * Implémentée dans `logger_ncurses.c`. Remplace la boucle prompt/getcmdline
 * classique : gère les fenêtres ncurses (sortie scrollable, zone Events, ligne
 * de saisie), lit les caractères au clavier, dispatche les commandes via
 * `do_command_line`. Ne retourne pas (appelle `exit` quand l'utilisateur quitte).
 */
void nc_console_loop(void);
#endif /* USE_NCURSES */

#endif /* logger_h */
