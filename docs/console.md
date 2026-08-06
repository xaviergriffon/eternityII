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
| `print [fichier]` | Affiche toutes les files au format JSON, ou les exporte dans `fichier` |
| `printFile N [fichier]` | Affiche le contenu de la file numéro `N`, ou l'exporte dans `fichier` |
| `printAnalysed [fichier]` | Affiche les possibilités en cours d'analyse, ou les exporte dans `fichier` |
| `statistic` | Affiche des statistiques sur le contenu des files (alias : `stats`) |
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
| `clientsCommand <ligne>` *(serveur)* | Pousse `<ligne>` à distance à tous les clients connectés (filtrée par une liste blanche : `pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch` ; alias : `clientsCmd`) |

Les commandes marquées comme « propagées aux enfants » (`backup`, `restore`,
`removeNoNext`, `limit`, `maxStockByThread`, `prunerBatch`, `min`, `printAnalysed`,
`pause`, `resume`) sont automatiquement retransmises à tous les processus fils via
socket Unix. Les commandes `clients*` sont **serveur uniquement** : elles agissent sur
le [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) distant, pas
sur des process fils locaux.

## Bandeau de stats live (mode ANSI)

Comme en ncurses, une ligne fixe en vidéo inverse, juste au-dessus de la zone
Events, affiche en continu les statistiques du thread checker (`coups/s`,
`stock`, `analyse`, `record`, …) — même mécanisme de région fixe que la zone
Events (`redraw_status_zone_locked`, `src/ui/logger.c`), rafraîchi via
`log_status()` sans perturber la région de défilement ni la ligne de saisie.
Avant le premier rapport, la ligne affiche un message d'attente. Sur un
terminal trop petit pour réserver la zone (ou sortie non interactive), le
bandeau est simplement absent — `check` reste la voie de consultation.

## Zone Events (en bas de l'écran)

Les évènements notables sont affichés dans une bande fixe en bas de la console, juste
au-dessus du prompt, sous le bandeau de stats :

```
┌──────────────────────────────────┐
│  Sortie des commandes, logs…     │
│  …                               │
│  commande : check                │
│  file:0 stock:0                  │
│  …                               │
├──────────────────────────────────┤
│  coups/s:… stock:… record:…      │  ← bandeau de stats, fixe (vidéo inverse)
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
| Molette souris | Remonte / descend de 3 lignes (descendre jusqu'en bas réactive le suivi automatique) |
| `Entrée` | Réactive le suivi automatique du bas |
| `Ctrl-L` | Efface la vue (comme `clear`) — l'historique du pad reste accessible via `PgUp` |

> Souris activée oblige : le terminal intercepte les clics, la **sélection de
> texte** se fait alors avec `Maj` enfoncé (comportement standard des
> applications plein écran). La profondeur d'historique du pad (3000 lignes par
> défaut) est surchargeable à la compilation :
> `make NCURSES=1 CPPFLAGS="-DOUTPUT_PAD_LINES=10000"`.

Quand on n'est pas en bas, le titre de la zone Events affiche le nombre de lignes
cachées (`[+N sous la vue — PgDn/End pour revenir]`).

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
