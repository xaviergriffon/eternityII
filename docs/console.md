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

### Recherche & régulation

| Commande | Description |
|---|---|
| `pause` | Pause administrative de la recherche (`REQUEST_ADMIN_PAUSE`) — distincte de la pause de régulation de débit interne (`limit`), ne se lève que par `resume` ; diffuse aussi `CTRL_COMMAND "pause"` à tous les clients connectés (utile côté serveur, qui n'a pas de recherche locale à mettre en pause), et persiste l'état pour les clients qui se connecteront après |
| `resume` | Lève une pause administrative posée par `pause` ; diffuse aussi `CTRL_COMMAND "resume"` à tous les clients connectés et efface l'état persisté |
| `limit N` | Limite la vitesse de recherche à `N` essais/seconde (0 = illimité) |
| `maxStockByThread N` | Ajuste le stock max par thread à la volée |
| `prunerBatch N` | Ajuste la taille de lot d'échange du pruner à la volée (borné à [1, 65536]) |

### Stock & files

| Commande | Description |
|---|---|
| `sortAsc` | Trie les possibilités par ordre croissant (moins avancées en premier ; alias : `sorta`) |
| `sortDesc [n]` | Trie par ordre décroissant (plus avancées en premier) ; `n` pour une file spécifique (alias : `sortd`) |
| `sortDescMulti` | Trie toutes les files en parallèle (alias : `sortdm`) |
| `split` | Répartit les possibilités entre les 10 files |
| `regroup` | Regroupe toutes les files en une seule |
| `removeNoNext` | Supprime les possibilités sans continuation possible (élagage ; alias : `rmnonext`, `prune`) |
| `expand N` | Développe le stock jusqu'au niveau de curseur `N` ([anti-famine](utilisation.md#expansion-du-stock-au-démarrage---expand-level-anti-famine), borné à 4 passes / `EXPAND_MAX_STOCK` possibilités) |
| `restockAnalysed` | Remet les possibilités en cours d'analyse dans le stock |
| `min` | Affiche le niveau minimum dans les files |

### Sauvegarde & restauration

| Commande | Description |
|---|---|
| `backup` | Sauvegarde les files de possibilités dans `eternityII.back` et `eternityII-in_analyse.back` |
| `restore [fichier [fichier_analyse]]` | Restaure les files depuis les fichiers `.back` (remplace le stock) |
| `import` | Importe des possibilités depuis les fichiers `.back` dans les files courantes |
| `loadJson` | Importe une possibilité depuis une chaîne JSON (équivalent de `import` pour le format JSON) |

### Diagnostic & vérification

| Commande | Description |
|---|---|
| `check` | Affiche le dernier rapport de statistiques (n'efface plus l'écran — utiliser `clear`) |
| `print` | Affiche toutes les files au format JSON |
| `printFile N` | Affiche le contenu de la file numéro `N` |
| `printAnalysed` | Affiche les possibilités en cours d'analyse |
| `statistic` | Affiche des statistiques sur le contenu des files (alias : `stats`) |
| `checkDatas` | Vérifie l'intégrité des possibilités |
| `checkDuplicate` | Recherche les doublons dans les files |
| `checkFiles` | Vérifie l'intégrité de toutes les files |
| `checkFile N` | Vérifie la file numéro `N` |
| `checkDirections` | Vérifie la cohérence des directions de parcours |

### Pilotage des clients (serveur)

| Commande | Description |
|---|---|
| `clients` *(serveur)* | Liste les sessions de [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) actives (pid, forks, mode, dernière activité) |
| `clientsStats` *(serveur)* | Demande les statistiques agrégées de chaque client connecté via son canal de contrôle (équivalent de `POST /api/v1/clients/stats` sur l'[API HTTP](api_http_rest.md)) |
| `clientsCommand <ligne>` *(serveur)* | Pousse `<ligne>` à distance à tous les clients connectés (filtrée par une liste blanche : `pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch` ; alias : `clientsCmd`) |

Les commandes marquées comme « propagées aux enfants » (`backup`, `restore`,
`removeNoNext`, `limit`, `maxStockByThread`, `prunerBatch`, `min`, `printAnalysed`,
`pause`, `resume`) sont automatiquement retransmises à tous les processus fils via
socket Unix. Les commandes `clients*` sont **serveur uniquement** : elles agissent sur
le [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) distant, pas
sur des process fils locaux.

## Zone Events (en bas de l'écran)

Les évènements notables sont affichés dans une bande fixe en bas de la console, juste
au-dessus du prompt :

```
┌──────────────────────────────────┐
│  Sortie des commandes, logs…     │
│  …                               │
│  commande : check                │
│  file:0 stock:0                  │
│  …                               │
├──────────────────────────────────┤
│  Events                          │  ← bande inversée, fixe
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

## Ligne de saisie protégée des logs asynchrones (mode ANSI)

Pendant la frappe, les logs asynchrones (thread de statistiques, événements
relayés des processus de recherche) n'écrasent plus la ligne de saisie : chaque
log terminé par un saut de ligne efface la ligne en cours, s'affiche, puis la
ligne `commande : …` est **redessinée en dessous** avec la saisie intacte. (En
ncurses le problème ne se posait pas : la saisie vit dans une fenêtre dédiée.)

## Historique des commandes (flèches ↑ / ↓)

Les 100 dernières commandes saisies sont conservées en mémoire pour la session. Les
touches ↑ et ↓ rappellent les commandes précédentes (comme dans bash) :

| Touche | Effet |
|---|---|
| ↑ | Rappelle la commande précédente (la première pression mémorise la saisie en cours pour pouvoir y revenir) |
| ↓ | Avance vers les commandes plus récentes ; revient à la saisie en cours en bas |
| Entrée | Exécute la commande et l'ajoute à l'historique (dédoublonnage si identique à la précédente) |
| Backspace | Efface le dernier caractère |
| Ctrl-L | Efface l'écran (comme la commande `clear`) et redessine la saisie en cours |

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
│  Events  [+47 sous la vue]         │
│  …                                 │
├────────────────────────────────────┤
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
| `Entrée` | Réactive le suivi automatique du bas |
| `Ctrl-L` | Efface la vue (comme `clear`) — l'historique du pad reste accessible via `PgUp` |

Quand on n'est pas en bas, le titre de la zone Events affiche le nombre de lignes
cachées (`[+N sous la vue — PgDn/End pour revenir]`).

Le build par défaut (sans `NCURSES=1`) reste 100 % fonctionnel et **sans aucune
dépendance** sur ncurses.

## Voir aussi

- [Utilisation](utilisation.md) — modes d'exécution et fichiers générés.
- [Échanges client / serveur](echanges_client_serveur.md#canal-de-contrôle-v9) — canal de contrôle piloté par les commandes `clients*`.
- [API HTTP REST admin](api_http_rest.md) — équivalents HTTP des commandes admin.
