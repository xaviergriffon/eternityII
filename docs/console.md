# Console interactive

Une fois lancé (quel que soit le mode), le programme écoute des commandes sur
l'entrée standard. En cas de faute de frappe, le programme propose automatiquement
la commande la plus proche (`vouliez-vous dire "sortDesc" ?`).

## Aide intégrée

- `help` — liste toutes les commandes **groupées par catégorie**, avec leur syntaxe
  et un résumé d'une ligne.
- `help <commande>` — détail d'une commande : usage, catégorie, portée
  (serveur/client), propagation aux processus fils, et complément d'explication.
- `help <catégorie>` — n'affiche que la section demandée : `general`, `recherche`,
  `stock`, `sauvegarde`, `diagnostic` ou `clients`.

Les noms canoniques sont en **camelCase complet** (`sortAsc`, `removeNoNext`,
`clientsCommand`, …) et **insensibles à la casse** (`maxstockbythread` fonctionne).
Les noms historiques abrégés restent acceptés comme **alias** : `sorta` (sortAsc),
`sortd` (sortDesc), `sortdm` (sortDescMulti), `rmnonext` et `prune` (removeNoNext),
`clientsCmd` (clientsCommand), plus `?` (help), `quit` (exit), `stats` (statistic).
Une commande appelée avec un argument manquant affiche automatiquement son rappel
d'usage (`usage : limit <n> — …`) au lieu d'échouer en silence.

Le code correspondant vit dans [src/ui/console.c](../src/ui/console.c),
[src/ui/command_lines.c](../src/ui/command_lines.c) (interpréteurs),
[src/ui/command_match.c](../src/ui/command_match.c) (suggestion Levenshtein),
[src/ui/command_history.c](../src/ui/command_history.c) (historique) et
[src/ui/logger.c](../src/ui/logger.c) / [logger_ncurses.c](../src/ui/logger_ncurses.c)
(affichage).

## Commandes

Les commandes sont présentées ici par catégorie, comme dans `help`.

### Général

| Commande | Description |
|---|---|
| `help [commande\|catégorie]` | Affiche l'aide générale, le détail d'une commande, ou une seule catégorie (alias : `?`) |
| `exit` | Arrête proprement le programme (alias : `quit`) |
| `clear` | Efface l'écran sans perdre le contenu — poussé dans le scrollback natif en ANSI, accessible via `PgUp` en ncurses (alias : `cls` ; raccourci : `Ctrl-L`) |
| `config` *(client/pruner)* | Sans argument : affiche l'état de l'orchestrateur de démarrage différé (`WAITING_CONFIG`/`COUNTDOWN`/`CONFIGURING`/`RUNNING`/…, avec le temps restant avant auto-démarrage en `COUNTDOWN`), la configuration **effective** (`nb_forks`, `server_host`, `parts_file`, `max_stock_by_thread`, `limit`, `pruner_batch`, `dfs_budget` — reflète les globales courantes, y compris un `limit`/`maxStockByThread`/`prunerBatch`/`prunerDfsBudget` déjà exécuté depuis la console) et la configuration **en préparation**. N'annule pas le décompte |
| `config <clé> <valeur>` *(client/pruner)* | Écrit `<clé> = <valeur>` dans la configuration **en préparation** (mêmes clés que le fichier `--config-file`) et **annule définitivement** le décompte d'auto-démarrage s'il était en cours — comme n'importe quelle frappe au clavier pendant le décompte, cf. plus bas. `start` (manuel ou déclenché par un décompte qui va à son terme) applique cette configuration en préparation aux globales AVANT de forker — pas besoin de redémarrer le process pour qu'une valeur préparée prenne effet |
| `configSave` *(client/pruner)* | Écrit la configuration effective dans le fichier de configuration, avec toute valeur **en préparation** superposée par-dessus (écriture atomique `.tmp` puis `rename`, comme `backup`) — défaut `./eternityii-client.conf`, option `--config-file <chemin>`. C'est ainsi qu'une valeur préparée par `config <clé> <valeur>` finit par prendre effet, au prochain démarrage |
| `start` *(client/pruner)* | Fork immédiat des process de recherche avec la configuration **effective**, sans attendre un éventuel décompte (`COUNTDOWN`) — même chemin de code que ce décompte à échéance. Erreur explicite si déjà en cours d'exécution |
| `stopForks` *(client/pruner)* | Arrête les process de recherche **sans quitter ce process** (console, canal de contrôle, API HTTP restent actifs). SIGINT à chaque fils, puis escalade SIGTERM (+5s) et SIGKILL (+10s) si nécessaire — jamais bloquant plus de ~10s. Erreur explicite si aucun fork n'est en cours d'exécution. Retour à `WAITING_CONFIG` ensuite : un nouveau `start` (ou `config`+`configApply`) est nécessaire pour relancer |
| `configApply` *(client/pruner)* | Applique la configuration **en préparation** (`config <clé> <valeur>`) aux fils déjà en cours d'exécution — erreur explicite si aucun fork n'est en cours. Si seules des clés à chaud (`max_stock_by_thread`/`limit`/`pruner_batch`/`dfs_budget`) sont préparées : appliquées immédiatement et diffusées par IPC aux fils, sans interruption de la recherche. Si `nb_forks`/`server_host`/`parts_file` est préparé (avec une valeur différente de l'effective) : équivalent à `stopForks` suivi d'une reconstruction (tableaux de fils et/ou map de recherche partagée) puis d'un re-fork automatique avec la nouvelle configuration |

> **Décompte d'auto-démarrage (`COUNTDOWN`)** : 5 s ne suffisent pas à taper
> une commande — dès la première touche pressée à l'invite, le décompte est
> annulé (état `CONFIGURING`), qu'elle appartienne ou non à une commande
> `config` valide. La configuration effective chargée est affichée dans les
> logs avant le décompte, pour juger sans rien taper s'il faut l'interrompre.

### Recherche & régulation

| Commande | Description |
|---|---|
| `pause` | Pause administrative de la recherche (`REQUEST_ADMIN_PAUSE`) — distincte de la pause de régulation de débit interne (`limit`), ne se lève que par `resume` ; diffuse aussi `CTRL_COMMAND "pause"` à tous les clients connectés (utile côté serveur, qui n'a pas de recherche locale à mettre en pause), et persiste l'état pour les clients qui se connecteront après |
| `resume` | Lève une pause administrative posée par `pause` ; diffuse aussi `CTRL_COMMAND "resume"` à tous les clients connectés et efface l'état persisté |
| `limit N` | Limite la vitesse de recherche à `N` essais/seconde (0 = illimité) |
| `maxStockByThread N` | Ajuste le stock max par thread à la volée |
| `prunerBatch N` | Ajuste la taille de lot d'échange du pruner à la volée (borné à [1, 65536]) |
| `prunerDfsBudget N` | Ajuste à la volée le budget de nœuds de la preuve de fermeture bornée du pruner (§4.6b) : une possibilité jugée vivante par le contrôle superficiel mais pas encore `checked` est rejouée par un backtracking réel plafonné à `N` nœuds — si ce budget suffit à épuiser tout son sous-arbre, elle est prouvée morte (aucun faux positif) et jamais redistribuée, sinon comportement inchangé (conservée, `checked`). `N <= 0` désactive ce contrôle supplémentaire (comme `limit 0`) ; borné à [0, 10000000] ; **désactivé par défaut** (0) — mesuré sans aucun gain sur le stock réel actuel (mur structurel `max_result` ≈ 74/256 : les possibilités atteignables aujourd'hui laissent encore trop de cases vides pour qu'un budget raisonnable les épuise), opt-in à activer si une évolution ultérieure de la recherche déplace ce mur (voir §4.6b, docs/conception/elagage_recherche.md) |

### Stock & files

| Commande | Description |
|---|---|
| `sortAsc` | Trie les possibilités par ordre croissant (moins avancées en premier ; alias : `sorta`) |
| `sortDesc [n]` | Trie par ordre décroissant (plus avancées en premier) ; `n` pour une file spécifique (alias : `sortd`) |
| `sortDescMulti` | Trie toutes les files en parallèle (alias : `sortdm`) |
| `split` | Répartit les possibilités entre les 10 files |
| `regroup` | Regroupe toutes les files en une seule |
| `removeNoNext` | Supprime les possibilités sans continuation possible (élagage ; alias : `rmnonext`, `prune`) |
| `expand N` | Développe le stock jusqu'à `N` pièces posées ([anti-famine](utilisation.md#expansion-du-stock-au-démarrage---expand-level-anti-famine), borné à `expand_max_levels` passes (défaut 4, réglable via `--expand-max-levels`) / `expand_max_stock` possibilités (défaut 100000, réglable via `--expand-max-stock`)) |
| `restockAnalysed` | Remet les possibilités en cours d'analyse dans le stock |
| `rebalance [n]` | Rééquilibre le stock d'un seul pas incrémental (file la plus pleine → la plus vide, `n` possibilités par pool, défaut `rebalance_budget` réglable via `--rebalance-budget`) — le même appel que celui automatique de chaque tour serveur (10 s), déclenché immédiatement ; contrairement à `split`, ne redistribue pas intégralement en un appel |
| `stockMemory` | Affiche le [plafond RAM du stock](utilisation.md#plafond-ram-du-stock---stock-max-ram) et l'occupation actuelle (Mo, possibilités) — deux pools comptés ensemble, jamais le pool analysé — ainsi que le [débordement sur disque](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir) (toujours affiché, même à 0) et le total résident + déporté |
| `stockMaxRam <mo>` | Fixe à chaud le [plafond RAM du stock](utilisation.md#plafond-ram-du-stock---stock-max-ram) (équivalent de `--stock-max-ram`) ; `<mo> <= 0` désactive le plafond (illimité) |
| `spill [n]` | Déclenche immédiatement un pas de [débordement/rechargement sur disque](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir) — équivalent du tick automatique du thread de débordement (100 ms) ; `n` optionnel : budget de possibilités pour ce pas (défaut 4096) |
| `min` | Affiche le niveau minimum dans les files |

### Sauvegarde & restauration

| Commande | Description |
|---|---|
| `backup` | Sauvegarde les files de possibilités dans `eternityII.back` et `eternityII-in_analyse.back` (stock et pool analysé capturés à un instant T unique, gelés puis libérés progressivement file par file), ainsi que le meilleur plateau connu (`eternityII-best_board.back`), le cumul par machine (`eternityII-known_clients.back`, voir [Registre de clients connus](echanges_client_serveur.md#registre-de-clients-connus)) et, si [`--stock-spill-dir`](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir) est actif, un cliché incrémental des segments de débordement (`<--stock-spill-dir>/snapshot/`) — chaque fichier/segment n'est réécrit que si son propre contenu a changé depuis la dernière écriture |
| `restore [fichier [fichier_analyse]]` | Restaure les files depuis les fichiers `.back` (remplace le stock) ; recharge aussi, sans argument dédié, le meilleur plateau connu, le cumul par machine et le cliché de débordement le plus récent (`<--stock-spill-dir>/snapshot/`, absence tolérée dans les trois cas). Si le débordement restauré ne correspond pas à ce qui a été sauvegardé (`<fichier>.spillcount`) — `--stock-spill-dir` oublié/différent, cliché supprimé — la commande **échoue explicitement** plutôt que de le taire, voir [Utilisation](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir) |
| `import` | Importe des possibilités depuis les fichiers `.back` dans les files courantes |
| `loadJson` | Importe une possibilité depuis une chaîne JSON (équivalent de `import` pour le format JSON) |

### Diagnostic & vérification

| Commande | Description |
|---|---|
| `check` | Affiche le dernier rapport de statistiques (n'efface plus l'écran — utiliser `clear`) |
| `print [fichier]` | Affiche toutes les files au format JSON, ou les exporte dans `fichier` |
| `printFile N [fichier]` | Affiche le contenu de la file numéro `N`, ou l'exporte dans `fichier` |
| `printAnalysed [fichier]` | Affiche les possibilités en cours d'analyse, ou les exporte dans `fichier` |
| `statistic` | Affiche la répartition du stock par nombre de pièces posées (`alloc`, voir [le champ `alloc`](api_http_rest.md#le-champ-alloc)), pools non vérifié et vérifié confondus, avec la difficulté moyenne (`min_candidats`, voir [le champ `min_candidats`](api_http_rest.md#le-champ-min_candidats)) quand elle est mesurée pour ce niveau (alias : `stats`). Non exécutable à distance (ni canal de contrôle, ni `POST /api/v1/command`) puisqu'elle n'écrit que dans les journaux ; l'équivalent exploitable par une application tierce est [`GET /api/v1/stock-distribution`](api_http_rest.md#get-apiv1stock-distribution), qui expose la même donnée en JSON, en distinguant en plus le pool « en cours d'analyse » |
| `checkDatas` | Vérifie l'intégrité des possibilités |
| `checkDuplicate` | Recherche les doublons dans les files |
| `checkFiles` | Vérifie l'intégrité de toutes les files |
| `checkFile N` | Vérifie la file numéro `N` |
| `checkDirections` | Vérifie la cohérence des directions de parcours |

### Pilotage des clients (serveur)

| Commande | Description |
|---|---|
| `clients` *(serveur)* | Liste les sessions de [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) actives (session_no, libellé déclaré, pid, IP du pair, mode, forks, machine_uid/client_uid, dernière activité) |
| `clientsStats` *(serveur)* | Demande les statistiques agrégées de chaque client connecté via son canal de contrôle (équivalent de `POST /api/v1/clients/stats` sur l'[API HTTP](api_http_rest.md)) |
| `clientsCommand [--to <session_no\|client_uid\|label>] <ligne>` *(serveur)* | Pousse `<ligne>` à distance, filtrée par la même liste blanche (`control_command_allowed` : `pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch`, `prunerDfsBudget`, `clientsCommand`/`clientsCmd`, `clientsWork`, `start`, `stopForks`, `configApply`, `config`, `configSave` ; alias : `clientsCmd`). Sans `--to` : diffusion à tous les clients connectés (comportement historique). Avec `--to <cible>` : n'atteint QUE la session désignée par son `session_no` (entier, voir `clients`), son `client_uid` (hexadécimal complet) ou son `label` déclaré — jamais d'espace dans la cible. Une cible inconnue/déconnectée ou un `label` partagé par plusieurs sessions est refusé, jamais redirigé vers un autre client (voir [Adressage des commandes](echanges_client_serveur.md#adressage-des-commandes---to)). Sur la console, aucune authentification (l'accès shell fait foi) ; également exécutable via l'[API HTTP admin](api_http_rest.md#post-apiv1command) (`POST /api/v1/command`), où elle exige, elle, un jeton Bearer valide (c'est une commande de modification — voir [Authentification](api_http_rest.md#authentification)). `start`/`stopForks`/`configApply`/`config`/`config <clé> <valeur>`/`configSave` (voir [Pilotage à distance du cycle de vie des fils](echanges_client_serveur.md#pilotage-à-distance-du-cycle-de-vie-des-fils)) pilotent ainsi à distance le cycle de vie des fils d'un client précis — ex. `clientsCommand --to jetson-1 stopForks` puis `clientsCommand --to jetson-1 configApply` après un `clientsCommand --to jetson-1 config nb_forks 8` ; `exit` reste et restera hors de cette liste |
| `knownClients` *(serveur)* | Liste les machines **connues** (registre de cumul, distinct de `clients` : survit à la déconnexion **et**, depuis PR5, à un redémarrage du serveur via `backup`/`restore`) — `machine_uid`, dernier libellé/IP/mode déclarés, statut connecté/déconnecté, nombre de sessions actives et de connexions cumulées, cumul pruner (checked/removed), meilleur résultat jamais rapporté, dernière activité (voir [Registre de clients connus](echanges_client_serveur.md#registre-de-clients-connus)) |
| `clientsWork <session_no\|client_uid\|label>` *(serveur)* | Affiche ce qu'un client précis détient actuellement dans le pool « en cours d'analyse » (nombre de possibilités, `alloc` maximal), ou qu'il n'en détient aucune. Cible résolue **exactement** comme `clientsCommand --to` (refusée si inconnue, déconnectée ou ambiguë) mais lecture pure : aucune commande n'est envoyée au client, seule l'attribution déjà enregistrée côté serveur est consultée (voir [Attribution des analyses en cours](echanges_client_serveur.md#attribution-des-analyses-en-cours)). Également exécutable via l'[API HTTP admin](api_http_rest.md#post-apiv1command) (`POST /api/v1/command`) — le résultat n'apparaît alors que dans les journaux du serveur, la réponse HTTP restant `{"result":"ok"}` |
| `leaseDuration <n>` *(serveur)* | Fixe la durée (secondes) du bail à expiration des possibilités attribuées à un client : passé ce délai ET si le client n'a plus de session de contrôle active (déconnexion confirmée), une possibilité est rendue automatiquement au stock non vérifié — un client mort (`kill -9`, coupure réseau, panne) ne gèle plus sa part indéfiniment. Un client toujours connecté n'expire **jamais**, quelle que soit la durée d'analyse : ce délai n'est qu'un minorant, pas un budget garanti. `<n> <= 0` désactive le bail (comme `limit 0` pour la régulation de débit). N'affecte que les possibilités attribuées après ce changement ; défaut 300 s (voir [Bail à expiration des analyses en cours](echanges_client_serveur.md#bail-à-expiration-des-analyses-en-cours)) |

Les commandes marquées comme « propagées aux enfants » (`backup`, `restore`,
`removeNoNext`, `limit`, `maxStockByThread`, `prunerBatch`, `prunerDfsBudget`, `min`, `printAnalysed`,
`pause`, `resume`) sont automatiquement retransmises à tous les processus fils via
socket Unix. Les commandes `clients*` sont **serveur uniquement** : elles agissent sur
le [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) distant, pas
sur des process fils locaux.

`config`, `configSave`, `start`, `stopForks` et `configApply` sont, à l'inverse,
**masquées côté serveur** : ni listées dans `help`, ni exécutables (`commande
inconnue`), ni suggérées en cas de faute de frappe — contrairement aux commandes
`*(serveur)*` ci-dessus, exécutées sans effet (no-op inoffensif) sur un client, ces
cinq commandes agiraient sur les globales/l'orchestrateur du *serveur*
(`NB_THREADS` y désigne la taille du pool de connexions, pas un nombre de forks ;
`fork_orchestrator_run` n'est de toute façon appelée que par `handle_client`) si
elles n'étaient pas bloquées, produisant un fichier de configuration trompeur ou un
événement sans effet observable plutôt qu'un no-op sans conséquence. Ce masquage ne
s'applique qu'à l'exécution **directe** sur la console d'un serveur : poussées à
distance vers un CLIENT via `clientsCommand` (ligne ci-dessus), elles s'exécutent
normalement — c'est le rôle du serveur qui reçoit `clientsCommand`, jamais celui du
process qui exécute in fine la commande, qui décide.

## Bandeau de stats live (mode ANSI)

Une ligne fixe en vidéo inverse, juste au-dessus de la liste des événements,
affiche en continu les statistiques du thread checker (`coups/s`, `stock`,
`analyse`, `record`, …) — `redraw_status_zone_locked`, `src/ui/logger.c`,
rafraîchi via `log_status()` sans perturber la région de défilement ni la
ligne de saisie. Avant le premier rapport, la ligne affiche un message
d'attente. Sur un terminal trop petit pour réserver la zone (ou sortie non
interactive), le bandeau est simplement absent — `check` reste la voie de
consultation.

**Pas de titre « Events » : le bandeau lui-même fait déjà la séparation
visuelle** avec la région de défilement au-dessus, et le format horodaté des
lignes qui suivent (`[hh:mm:ss] ...`) suffit à les identifier comme des logs
— un titre y était redondant (retiré aussi bien côté ANSI que ncurses, voir
la note ci-dessous). Le bandeau occupe donc toute la largeur du terminal avec
uniquement le texte des stats, complété d'espaces jusqu'au bord.

**Positionnement : une rangée sous la région de défilement, jamais dessus.**
La dernière rangée de la région de défilement (`\033[1;Nr`) est celle où vit
la ligne de commande, qui y reste ancrée par le mécanisme de scroll — le
bandeau doit donc être calculé UNE rangée plus bas (`redraw_status_zone_locked`,
`src/ui/logger.c`), jamais sur cette même rangée : un bandeau mal placé s'y
écrirait, invisible car aussitôt recouvert par le redessin de la ligne de
saisie — exactement le symptôme observé avant ce correctif (bandeau/« Events »
disparaissant, saisie apparemment « sur la même ligne » que les stats).

**Le redessin n'a lieu que sur changement réel, jamais en tâche de fond à
cadence fixe.** `log_status()`/`log_event()` redessinent déjà, chacun,
immédiatement quand leur propre contenu change. Le thread de fond
`event_zone_loop` (`src/ui/logger.c`), qui tourne toutes les secondes, ne
sert donc qu'à détecter un redimensionnement de terminal (`SIGWINCH` n'étant
pas exploité côté ANSI, contrairement à la variante ncurses) — il ne
redessine la zone que si la taille a effectivement changé, jamais de façon
inconditionnelle à chaque tick (source du scintillement pendant la saisie qui
a motivé ce correctif).

Ce dernier point (redessin conditionnel plutôt qu'à cadence fixe) ne concerne
que le mode ANSI par défaut : la variante ncurses (`src/ui/logger_ncurses.c`)
garde `stats_win`/`events_win` sur deux fenêtres distinctes et redessine déjà
de façon purement événementielle (pas de heartbeat périodique), donc sans le
scintillement observé côté ANSI. Le retrait du titre « Events », en revanche,
s'applique aux deux variantes : côté ncurses, la rangée de titre au-dessus
des événements ne s'affiche plus (en vidéo inverse) que lorsqu'on a remonté
dans l'historique du pad de sortie (PgUp), pour signaler qu'il y a du contenu
plus récent hors vue (`+N lignes sous la vue — PgDn/End pour revenir`) — sinon
elle reste simplement vide.

## Zone Events (en bas de l'écran)

Les évènements notables sont affichés dans une bande fixe en bas de la console, juste
au-dessus du prompt, sous le bandeau de stats (voir ci-dessus) :

```
┌──────────────────────────────────┐
│  Sortie des commandes, logs…     │
│  …                               │
│  commande : check                │
│  file:0 stock:0                  │
│  …                               │
├──────────────────────────────────┤
│  coups/s:… stock:… record:…       │ ← bandeau de stats, fixe (vidéo inverse)
│  [21:29:30] new record: 65 …     │
│  [21:30:15] SOLUTION FOUND! …    │
│  …                               │
└──────────────────────────────────┘
│  commande : _                    │
```

Les évènements suivants sont câblés :

| Évènement | Source |
|---|---|
| `new record: N pieces placed` | Détecté à chaque tick du checker (10 s) quand `max_result` augmente |
| `request unfulfilled: all threads busy` | Côté serveur, quand un nouveau client se connecte mais aucun thread libre |
| `SOLUTION FOUND! (N pieces) - saved to ./solution_<pid>` | Émis depuis `checkIfResultFound` quand les 256 pièces sont placées |
| `nouveau client connecté` | Côté serveur, à chaque connexion TCP acceptée |
| `client déconnecté (…)` | Côté serveur, en fin de session : `fin de session` (propre), `connexion perdue` (brutale) ou `protocole interrompu` |
| `client rejeté : version …` | Côté serveur, quand le handshake de version échoue (version incompatible ou requête sans handshake valide) |
| `session de contrôle enregistrée (pid=…) -> slot N` | Côté serveur, quand un client annonce son [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) (`INST_CONTROL_HELLO`) |
| `session de contrôle déconnectée (slot N)` | Côté serveur, à la fin d'une session de canal de contrôle |
| `commande distante "…" exécutée (code retour N)` | Côté serveur, après qu'une commande `clientsCommand` ou `pause`/`resume` (diffusion) a été acquittée par le client |

Tout évènement est **horodaté et écrit dans `events.log`** (en plus de l'affichage
dans la zone), ce qui permet de garder une trace persistante hors session :

```sh
tail -f events.log
```

**Les erreurs (`log_error`/`log_errno`) sont aussi persistées dans `events.log`**,
horodatées de la même façon — sans pour autant apparaître dans la bande fixe
"Events" ci-dessus ni dans le buffer circulaire qu'elle affiche, pour ne pas
noyer les évènements notables (records, connexions…) sous des erreurs répétées.
Avant cela, une erreur (ex. écriture de fichier échouée, désynchronisation
protocolaire) n'était visible que sur `stderr`/le pad de sortie de la console —
perdue une fois sortie du scrollback ou d'une session non interactive. Un
enfant forké relaie son erreur au process PARENT par IPC (comme pour les
autres logs) ; c'est ce dernier qui écrit effectivement la ligne dans
`events.log`, pour éviter plusieurs écrivains concurrents sur le fichier.

**Le détail système (errno) fait partie de la même ligne.** Les points
d'échec d'E/S (`read_parts`, `save_possibility`, `backup`/`backup_analysed`,
`import`/`restore`(`_analysed`), `configSave`) utilisaient `perror()` pour
afficher le détail (`fopen(): No such file or directory`) : cet appel écrit
directement sur `stderr`, en contournant entièrement le logger — ni routé par
IPC pour un enfant forké (risque de désynchronisation avec le reste de
l'affichage console), ni jamais écrit dans `events.log`. Ces 10 sites
utilisent désormais `log_errno()`, qui produit une seule ligne journalisée
avec le contexte ET le détail errno (`read_parts file :chemin 2 : No such
file or directory`), correctement routée et donc bien présente dans
`events.log`.

**La configuration effective de démarrage est journalisée UNIQUEMENT dans
`events.log`, jamais dans la zone d'événements ni sur la console.** Une
nouvelle fonction, `log_file()` (`src/ui/logger.h`), sert exactement ce cas :
écrire dans `events.log` sans afficher, contrairement à `log_event` (bornée à
200 octets, dimensionnée pour tenir sur une ligne de la zone fixe) ou
`log_console`/`log_info` (jamais persistés). Réservée au process PARENT
(aucun routage IPC, à la différence des autres fonctions de ce fichier) :

- **Côté client/pruner** : dès qu'un (re)démarrage des fils de recherche
  réussit — `start` manuel, décompte automatique écoulé, ou redémarrage à
  chaud `configApply` — une ligne `démarrage : N fork(s) lancé(s) — pid=…
  version_protocole=… eternParts=… mode=… label="…" machine_uid=…
  client_uid=… stock_files=…` suivie du dump de la configuration effective
  (`nb_forks`, `server_host`, `parts_file`, `max_stock_by_thread`, `limit`,
  `pruner_batch`, `dfs_budget`) est écrite. Voir
  `log_startup_diagnostics` (`src/app/fork_orchestrator.c`).
- **Côté serveur** : une ligne équivalente est écrite juste avant que
  `runserver` ne commence à accepter des connexions — `démarrage serveur :
  pid=… version_protocole=… eternParts=… nb_threads=… fichier="…"
  stock_files=… tcp_timeout=…s stop_on_solution=… expand_level=…
  expand_max_stock=… expand_max_levels=… rebalance_budget=… http_port=…
  http_token=configuré|absent|n/a` (jamais la valeur du jeton lui-même). Voir
  `log_server_startup_diagnostics` (`src/app/etii_server.c`).

But : diagnostiquer après coup un déploiement (quelles options CLI étaient
réellement actives à cet instant précis) via `tail -f events.log`, sans
dépendre du scrollback de la console ni d'avoir pensé à lancer
`config`/`configSave` avant un incident.

## Effacement de l'écran : la commande `clear` (Ctrl-L)

La politique d'affichage est uniforme : **aucune commande n'efface l'écran
implicitement** — toutes les sorties (y compris `check`) défilent normalement.
L'effacement est explicite, via la commande `clear` (alias `cls`) ou le raccourci
`Ctrl-L`, et **ne détruit jamais le contenu** :

- **Mode ANSI** : le contenu visible est poussé dans le **scrollback natif du
  terminal** (molette, Cmd+↑), exactement comme les lignes qui défilent
  naturellement — la région de défilement ANSI commence en haut de l'écran, donc
  les sorties longues (`statistic`, `print`, …) y restent aussi capturées.
- **Mode ncurses** : la vue devient blanche mais le pad de sortie est conservé —
  `PgUp`/`Home` permettent de revenir sur tout l'historique (3000 lignes).

## Pagination des sorties longues : « --Suite-- » (mode ANSI)

En mode ANSI interactif (stdin **et** stdout sont des terminaux), la sortie de
chaque commande est **paginée** : dès qu'une page d'écran est remplie, l'affichage
marque une pause sur une invite en vidéo inverse :

```
--Suite-- (espace : page, entrée : ligne, q : dérouler)
```

| Touche | Effet |
|---|---|
| Espace (ou toute autre touche) | Affiche la page suivante |
| Entrée | Avance d'une seule ligne |
| `q` | Déroule le reste de la sortie sans pause (**rien n'est supprimé**) |

Plus besoin de compter sur le seul scrollback pour lire un `help`, `statistic`
ou `print` : la sortie attend le lecteur. Points de conception :

- **Seule la commande en cours est paginée.** Les logs des autres threads
  (statistiques, événements relayés des processus de recherche) ne sont ni
  paginés ni retenus : le verrou d'affichage est relâché pendant l'attente
  d'une touche, l'affichage asynchrone reste vivant et le serveur continue de
  servir ses clients pendant qu'un opérateur lit une page.
- La pagination est **automatiquement inactive** hors terminal (console pilotée
  par pipe ou redirection — les scripts d'intégration ne voient jamais de
  pause), en fallback cooked, et sur un écran de moins de ~4 lignes utiles.
- En ncurses la question ne se pose pas : le pad + `PgUp`/`PgDn`/molette
  permettent de relire la sortie a posteriori.

## Export vers fichier des sorties massives (`print`/`printFile`/`printAnalysed`)

`print`, `printFile <n>` et `printAnalysed` acceptent un **argument fichier
optionnel** : au lieu d'afficher le dump JSON dans la console, il est écrit dans
ce fichier.

```sh
print ./dump.json                  # tout le data manager
printFile 3 ./file3.json           # une seule file
printAnalysed ./analysed.json      # possibilités en cours d'analyse
```

Sans argument, le comportement historique (affichage console, désormais
paginé en ANSI) est inchangé. Avec un fichier, une ligne de confirmation
résume l'export : `print : export : N possibilités écrites dans ./dump.json`.

Cette variante existait déjà indirectement via la pagination (`--Suite--`) et
le pad ncurses, mais un gros stock (des dizaines de milliers de possibilités
sur le puzzle 256) dépasse vite les 3000 lignes du pad ncurses, qui déborde
alors **silencieusement** par le haut ; l'export règle le problème d'un coup et
produit en prime un artefact **greppable** (`grep '"alloc": 42' dump.json`).

> **`printAnalysed <fichier>` en mode client** : cette commande est propagée
> aux processus fils de recherche (comme `backup`, `limit`, …) — le texte de
> la commande, argument fichier compris, est rejoué **tel quel** par le parent
> ET par chaque fork. Sans précaution, tous écriraient dans le même fichier en
> concurrence. Le nom est donc **automatiquement suffixé du pid** en mode
> client (`./analysed.json_12345`), exactement comme `backup` suffixe déjà
> `eternityII.back`. Côté serveur (pas de forks de recherche), le chemin est
> utilisé tel quel. `print`/`printFile` ne sont pas concernées : elles ne sont
> jamais propagées aux fils.

## Ligne de saisie protégée des logs asynchrones (mode ANSI)

Pendant la frappe, les logs asynchrones (thread de statistiques, événements
relayés des processus de recherche) n'écrasent plus la ligne de saisie : chaque
log terminé par un saut de ligne efface la ligne en cours, s'affiche, puis la
ligne `commande : …` est **redessinée en dessous** avec la saisie intacte. (En
ncurses le problème ne se posait pas : la saisie vit dans une fenêtre dédiée.)

## Édition de la ligne de saisie

Les deux builds partagent désormais la même logique d'édition (module
`src/ui/line_edit.c`, sans dépendance d'affichage) : le curseur peut se déplacer
n'importe où dans la ligne, pas seulement en être ajouté/retiré en fin.

| Touche | Effet |
|---|---|
| ← / → | Déplace le curseur d'un caractère (gauche / droite) |
| Home / Ctrl-A | Curseur en début de ligne |
| End / Ctrl-E | Curseur en fin de ligne |
| Backspace | Efface le caractère **avant** le curseur |
| Suppr (Delete) | Efface le caractère **sous** le curseur |
| Ctrl-U | Efface toute la ligne |
| Ctrl-W | Efface le mot précédant le curseur (comme readline/bash) |
| Entrée | Exécute la commande et l'ajoute à l'historique (dédoublonnage si identique à la précédente) |
| Ctrl-L | Efface l'écran (comme la commande `clear`) et redessine la saisie en cours |

> En ncurses, `Home`/`End` restent réservées au **scroll du pad de sortie**
> (voir plus bas) : utilisez `Ctrl-A`/`Ctrl-E` pour le début/fin de la ligne de
> saisie dans ce build. En ANSI, `Home`/`End`/`Ctrl-A`/`Ctrl-E` sont
> équivalentes (aucun autre usage de ces touches).

## Historique des commandes (flèches ↑ / ↓)

Les 100 dernières commandes saisies sont conservées en mémoire pour la session. Les
touches ↑ et ↓ rappellent les commandes précédentes (comme dans bash) :

| Touche | Effet |
|---|---|
| ↑ | Rappelle la commande précédente (la première pression mémorise la saisie en cours pour pouvoir y revenir) |
| ↓ | Avance vers les commandes plus récentes ; revient à la saisie en cours en bas |

L'historique fonctionne dans les deux builds. En ANSI, le terminal est basculé en
mode non-canonique (`tcsetattr`) le temps de la session pour permettre l'interception
des séquences `\033[A` / `\033[B`. Le mode initial est restauré automatiquement à la
sortie. Si stdin n'est pas un TTY (sortie redirigée), le programme retombe sur la
lecture ligne-par-ligne classique.

L'historique est **persisté entre sessions** dans `~/.eternityII_history` (repli sur
`./.eternityII_history` si `$HOME` est absent). Il est chargé au démarrage de la
console — l'absence du fichier au premier lancement n'est pas une erreur — et réécrit
à la sortie propre (commande `exit` ou fin de stdin) dans les deux builds. L'écriture
est atomique (fichier temporaire `.tmp` + `rename`) pour ne jamais corrompre
l'historique existant si l'écriture échoue.

## Interface ncurses (optionnelle, `make NCURSES=1`)

Le build `NCURSES=1` (voir [Compilation](compilation.md)) remplace l'affichage ANSI
par une vraie interface ncurses à quatre zones :

```
┌────────────────────────────────────┐
│  output_pad (scrollable, 3000 l.)  │
│  …                                 │
├────────────────────────────────────┤
│  coups/s:… stock:… record:…        │  ← bandeau stats live (vidéo inverse)
├────────────────────────────────────┤
│  …                                 │  ← pas de titre : le bandeau de stats
│  …                                 │    juste au-dessus fait déjà la
├────────────────────────────────────┤    séparation
│  commande : _                      │
└────────────────────────────────────┘
```

Le **bandeau de stats** (mis à jour en continu par le thread checker) affiche :
`coups/s`, `stock`, `analyse`, `record` et `limite`. Il est rafraîchi via
`log_status()` et ne perturbe pas le défilement du pad de sortie.

Touches de navigation dans l'historique :

| Touche | Effet |
|---|---|
| `PgUp` / `PgDn` | Remonte / descend d'une page |
| `Home` / `End` | Tout en haut / tout en bas |
| Molette souris | Remonte / descend de 3 lignes (descendre jusqu'en bas réactive le suivi automatique) |
| `Entrée` | Réactive le suivi automatique du bas |
| `Ctrl-L` | Efface la vue (comme `clear`) — l'historique du pad reste accessible via `PgUp` |

> Souris activée oblige : le terminal intercepte les clics, la **sélection de
> texte** se fait alors avec `Maj` enfoncé (comportement standard des
> applications plein écran). La profondeur d'historique du pad (3000 lignes par
> défaut) est surchargeable à la compilation :
> `make NCURSES=1 CPPFLAGS="-DOUTPUT_PAD_LINES=10000"`.

La zone d'événements n'a pas de titre — le bandeau de stats juste au-dessus
fait déjà la séparation visuelle, et le format horodaté des lignes
(`[hh:mm:ss] ...`) suffit à les identifier comme des logs. Sa première rangée
reste néanmoins utilisée, en vidéo inverse, pour signaler qu'on a remonté
dans l'historique du pad de sortie (`PgUp`) : `+N lignes sous la vue —
PgDn/End pour revenir`. Une fois revenu en bas (`End`, ou tout simplement en
tapant `Entrée`), cette rangée redevient vide.

Le build par défaut (sans `NCURSES=1`) reste 100 % fonctionnel et **sans aucune
dépendance** sur ncurses.

## `log_error` : stderr en ANSI, pad de sortie en ncurses (divergence assumée)

Contrairement au reste de l'interface publique de `logger.h`/`logger_ncurses.c`,
`log_error`/`log_errno` n'ont **pas** le même flux de destination selon le build,
et ce n'est **pas un oubli** :

- **ANSI** écrit sur le vrai **stderr** du processus — `2>err.log` capture
  effectivement les erreurs séparément de la sortie normale, comme n'importe
  quel programme Unix classique.
- **ncurses** écrit dans le **pad de sortie** (comme `log_info`), pas sur un
  stderr réel. Impossible de faire autrement : dès que `initscr()` a pris le
  contrôle de l'écran, toute écriture brute sur stdout/stderr casserait
  l'affichage géré par curses (sauf redirection vers un fichier — mais alors
  l'erreur n'apparaîtrait plus du tout à l'écran, ce qui est pire pour un usage
  interactif). Rendre les erreurs visibles dans le pad, au même endroit que le
  reste du flux, est le compromis le moins mauvais pour ce mode.

Une redirection `2>err.log` ne se comporte donc pas pareil selon le build : elle
capture les erreurs en ANSI, mais reste vide en ncurses (tout part dans le pad,
sur stdout). C'est un choix délibéré plutôt qu'une incohérence à corriger — les
deux modes n'ont pas le même degré de liberté vis-à-vis du terminal.

## Voir aussi

- [Utilisation](utilisation.md) — modes d'exécution et fichiers générés.
- [Échanges client / serveur](echanges_client_serveur.md#canal-de-contrôle-v9) — canal de contrôle piloté par les commandes `clients*`.
- [API HTTP REST admin](api_http_rest.md) — équivalents HTTP des commandes admin.
