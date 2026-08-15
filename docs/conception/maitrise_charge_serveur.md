# Maîtrise de la charge serveur : verrous bornés, sauvegarde cohérente à libération progressive

**Statut : en cours d'implémentation.** PR 1 (§2), PR 2 (§3) et PR 3 (§4) **livrées**. PR 4
et 5 (§5, §6) en proposition, non implémentées.

## 1. Contexte et diagnostic

Reproduction fournie : serveur `--expand-level 9` (14 375 696 possibilités en stock, voir
`--expand-level` dans `AGENTS.md`), puis un pruner 4 forks par lots de 100 sur le même
poste. Au bout d'environ une minute, la communication client/serveur se dégrade :
déconnexions en rafale sur les 4 forks simultanément, acquittements de lot rejetés en
masse (`batch analysed : possibilité non retirée`), `Broken pipe`, `Connection reset by
peer`.

**Cause identifiée, avec preuves quantitatives** :

| Fait | Source |
|---|---|
| Stock après expansion : 14 375 696 possibilités | log serveur de la reproduction |
| Sauvegarde automatique (`temp.back`) résultante : 5,48 Gio / 10 218 838 paquets | fichier produit par la reproduction |
| RAM de la machine de test : 16 Go ; stock résident ≈ 9 Go | `sysctl hw.memsize`, `sizeof(struct possibility_packet)` = 576 o |
| `backup()` tenait `lock_all_file()` (20 mutex — les deux pools de stock) pendant toute la double traversée d'écriture | `src/core/datamanager.c` |
| Aucun `setvbuf` sur le `FILE*` de sauvegarde → tampon stdio par défaut → environ 1,4 million d'appels `write()` sous verrou | absent du code avant PR 1 |
| `tcp_timeout` par défaut : 10 s (`DEFAULT_TCP_TIMEOUT`) | `src/app/static_variables.h` |

Le maillon exact : trois boucles de `src/core/datamanager.c` — `scroll_from_pool`,
`put_to_pool`, `add_possibility_analysed_impl` — tournaient indéfiniment tant qu'aucun
`pthread_mutex_trylock` ne réussissait, sans aucune sortie. Tant que `backup()` détenait
les 20 verrous du stock, tout thread serveur servant un `INST_GET`/`INST_GET_TO_CHECK_BATCH`
ou encaissant un `INST_ADD` y restait bloqué ; le client, lui, expirait au bout de
`tcp_timeout`. Une sauvegarde de 5,5 Gio en pointer-chasing sur 9 Go de liste chaînée, sur
une machine de 16 Go qui pagine, dépasse largement ce budget : tous les forks du pruner
expiraient ensemble — signature d'un blocage global, et non d'une contention ordinaire
entre threads (qui se répartirait dans le temps, pas en rafale synchronisée). Le reste
s'ensuit mécaniquement : `requeue_last_sent_possibility` rend le lot en cours au stock à la
déconnexion, puis le pruner, une fois reconnecté, ré-acquitte un lot déjà repris — d'où les
acquittements massivement rejetés.

`rmnonext` est **hors de cause** dans cette reproduction précise : `rmnonext_pass` ne
s'exécute que si `get_active_threads() <= 0` (`src/app/etii_server.c`), or des clients
étaient actifs pendant toute la fenêtre observée. Il partage cependant le même défaut
structurel (verrou global, sortie non bornée) — non traité par cette série, laissé ouvert.

## 2. PR 1 — Aucune maintenance ne déconnecte plus personne (livrée)

**Principe** : les trois boucles d'attente active abandonnent après
`DATAMANAGER_TRYLOCK_MAX_SWEEPS` tentatives infructueuses (≈ 500 ms, très en-deçà de
`tcp_timeout`) plutôt que de bloquer indéfiniment. Ce n'est **pas** un changement de
protocole : une réponse « rien de disponible » (`K = 0`, ou `INST_ERROR` sur `INST_ADD`) est
une réponse normale et déjà supportée depuis la v7 — le client attend et repolle, exactement
comme sur un stock réellement vide. Aucun bump de `VERSION`.

- `scroll_from_pool` (`src/core/datamanager.c`) : sortie bornée → `result->size` reste à 0.
  Indiscernable, côté appelant, d'un pool vide.
- `put_to_pool` : sortie bornée → retourne 1 (échec, rien d'inséré) au lieu de 0.
  `put_to_local`/`add_possibility` propagent ce code ; le serveur répond `INST_ERROR` sur
  `INST_ADD` (branche déjà existante), que le client traite déjà en remettant la
  possibilité dans sa file locale (`put_to_server`, chemin déjà testé).
- `add_possibility_analysed_impl` : sortie bornée → retourne -1 (le contrat documenté par
  le prototype existant avant cette PR, jamais implémenté jusqu'ici). Le serveur
  (`record_possibility_analysed_for_client`, `src/app/etii_server.c`) propage cet échec :
  une possibilité dont l'enregistrement « en cours d'analyse » échoue **n'est pas servie**
  au client — elle est rendue au stock (`add_possibility`) plutôt que distribuée sans trace,
  ce qui l'aurait fait échapper au bail (PR7 déjà en place) et à
  `requeue_last_sent_possibility`. Factorisé dans `record_batch_analysed_for_client`, qui
  compacte le lot en place au nombre réellement enregistré avant l'envoi.
- `setvbuf(f, NULL, _IOFBF, 1 Mio)` sur le `FILE*` de sauvegarde : ~1,4 million d'appels
  `write()` ramenés à ~1 400.
- `--tcp-timeout <n>` : option CLI globale (serveur et client/pruner), soupape indépendante
  de ce qui précède pour un réseau plus lent ou un stock encore plus volumineux.

**Bornage du temps d'attente, pas du nombre de tentatives.** `add_possibility_analysed_impl`
a deux modes (balayage rotatif, `thread < 0` — côté serveur ; file fixe retentée,
`thread >= 0` — côté client, un fork sur sa propre file) dont le coût par tentative diffère
d'un facteur `NB_FILE_POSSIBILITY` : le compteur de sortie compte les `usleep()`
effectivement exécutés dans les deux modes, pas les tentatives, pour borner le même budget
d'horloge quel que soit le mode.

**Tests** (`tests/core/test_datamanager.c`) : pour chacune des trois boucles (quatre tests,
`add_possibility_analysed_impl` étant testée dans ses deux modes séparément), un thread
compagnon exécute l'opération pendant que le test tient le verrou correspondant **sans
jamais le relâcher** ; le test attend un multiple du budget nominal puis vérifie — via un
drapeau `volatile`, jamais via un `pthread_join` qui ferait pendre le test lui-même en cas
de régression — que le thread compagnon est déjà revenu, avec le code de retour attendu et
sans qu'aucune possibilité n'ait été perdue ni insérée en double. Complémentaire des tests
préexistants (`*_spins_until_lock_released`, `*_spins_both_modes`), qui prouvent l'inverse :
un verrou relâché **avant** le budget laisse l'opération aboutir normalement — ces tests
restent inchangés et continuent de passer (relâchement à 60 ms, bien sous le budget de
500 ms).

## 3. Conception retenue pour la sauvegarde (PR 2, livrée)

**Livrée telle que conçue ci-dessous**, avec un correctif de trajectoire trouvé en
implémentant : le PR1 mergé documentait un `setvbuf` sur le fichier de sauvegarde
(`backup()`) qui n'avait en réalité jamais été codé — corrigé au passage sur `backup()`,
`backup_analysed()` et le nouveau `consistent_backup()`.

`consistent_backup(stock_filename, analysed_filename, &out_analysed_status)`
(`src/core/datamanager.{c,h}`) est la nouvelle fonction, appelée aux cinq points de
production qui sauvegardaient auparavant stock et analysé l'un après l'autre :
l'autobackup (`check_server_step`), l'arrêt sur solution (deux points — le chemin normal
et celui de `remove_possibilities_with_no_next`), la commande console `backup` (et donc
l'API HTTP admin, qui délègue à `backup_interpreter`), et le dump de diagnostic
`backup_failed_exit` côté client. `backup()`/`backup_analysed()` restent inchangées comme
fonctions indépendantes — largement utilisées telles quelles par la suite de tests — mais
ne sont plus appelées en paire nulle part en production.

Vérifié en conditions réelles : sur le même scénario de reproduction que PR1 (stock
expansé, pruner 4 forks), un autobackup réel a capturé `temp_analysed.back` non vide
(le pool analysé contenait effectivement du travail en vol du pruner), sans aucune
déconnexion — confirmant que la fenêtre `maintenance` unique couvrant les deux pools
n'introduit pas de nouveau blocage par rapport à PR1.

**Contrainte imposée** : la sauvegarde doit rester une image cohérente à l'instant T,
portant sur l'ensemble des possibilités — stock **et** pool « analysé ».

### Arbitrage : gel global à T, puis libération progressive — pas un verrouillage paresseux

Une variante initialement envisagée — verrouiller une file à la fois, l'écrire, la
libérer, passer à la suivante — a été **écartée** : elle est incompatible avec la
cohérence à l'instant T. Une possibilité présente dans une file pas encore verrouillée,
servie à un client, dont les enfants sont réinsérés dans une file déjà écrite, n'apparaît
dans aucune des deux images. Le cas est réel, pas théorique : c'est exactement le cycle du
pruner (prendre un lot, réinsérer les survivants, acquitter le parent).

La conception retenue verrouille **toutes** les files des trois pools d'emblée à l'instant
T, puis écrit et **libère une par une** :

- Une file reste verrouillée en continu de T jusqu'à son écriture : rien ne peut en sortir
  avant. L'image de chaque file est exactement son contenu à T.
- **Sans deadlock par construction** : la sauvegarde acquiert tout en une phase et ne fait
  ensuite que relâcher — elle ne peut former de cycle ni avec `restore`/`split`/`sort` (qui
  bloquent sur `lock_all_file()` puis progressent au fil des libérations), ni avec les
  chemins clients (`trylock` uniquement, PR 1).
- La fenêtre de blocage total vaut le temps d'écriture d'**une seule** file, pas de la
  sauvegarde entière — la capacité remonte ensuite par paliers 1/N, 2/N, ...
- **Coût mémoire nul**, contrairement à un instantané par `fork()`/copie-sur-écriture (le
  `BGSAVE` de Redis, envisagé puis écarté) : la copie-sur-écriture peut approcher 2× le
  résident, intenable avec 9 Go de stock sur une machine de 16 Go, et rouvrirait le terrain
  du `fork()` en process multithread déjà payé une fois par ce projet (voir
  [`../investigations/blocage_fork_gate_release_quiesce.md`](../investigations/blocage_fork_gate_release_quiesce.md)).
  Contrepartie assumée : les clients sont dégradés (`K = 0` partiel, via PR 1) pendant la
  sauvegarde, là où `fork()` ne les aurait pas dégradés du tout.

### Le gel couvre les deux pools, pool analysé libéré en premier

Le pool « analysé » est gelé dans le **même** instant T que le stock — corrige un trou
préexistant (`backup_analysed` s'exécute aujourd'hui après `backup`, à un instant
différent : un parent acquitté dans l'intervalle disparaît avec ses enfants, déjà
réinsérés).

Cela tient les deux familles de verrous ensemble, ce que `datamanager.h` proscrit
aujourd'hui pour tout le reste du fichier. **Vérifié comme sûr** : aucun chemin de
`datamanager.c` ne détient un verrou d'une famille en acquérant l'autre —
`restock_analysed` et `datamanager_reclaim_expired_leases` relâchent l'un avant
d'acquérir l'autre. La sauvegarde serait donc la seule à détenir les deux familles
simultanément, et un interblocage exige deux parties détenant chacune l'une et voulant
l'autre : aucun cycle possible. Le commentaire existant décrit une *conséquence assumée*
(instantané non atomique), pas une nécessité anti-deadlock — levé pour ce cas précis, avec
cette justification.

Ordre imposé : le pool analysé est libéré **en premier**. Servir un `INST_GET` exige à la
fois un verrou de stock et un verrou analysé (`record_possibility_analysed_for_client`) ;
libérer d'abord le stock ne servirait à rien tant que le pool analysé reste gelé. Ce pool
ne contient que le travail en vol (souvent quasi vide) : son écriture est rapide, et la
libération progressive du stock retrouve alors tout son effet.

## 4. Rééquilibrage incrémental borné en temps (PR 3, livrée)

Ce qui rend un « temps de blocage ≤ 1 s par file » vrai : des files de tailles comparables.

`datamanager_rebalance_step(max_packets)` (`src/core/datamanager.{c,h}`) déplace, par pas
borné et un verrou de pool à la fois (jamais deux ensemble — même motif que
`restock_analysed`/`reclaim_expired_leases`), de la file la plus pleine vers la plus vide —
indépendamment pour le pool non vérifié et le pool vérifié. Le montant déplacé est plafonné
à la fois par `max_packets` et par ce qui suffit à amener la file source ou la file
destination exactement à la cible (`total/NB_FILE_POSSIBILITY`) — jamais de dépassement, et
un stock déjà équilibré ne bouge pas (évite un va-et-vient perpétuel pour de petites
variations dues au trafic concurrent). Appelé une fois par tour dans `check_server_step`
avec un budget modeste (`rebalance_budget`, réglable via `--rebalance-budget <n>`, défaut
1000) — jamais un chemin chaud.

`split_datas()` (l'ancienne version : `regroup_pool_nolock` + trois copies par paquet sous
`lock_all_file()`, inexploitable à l'échelle de plusieurs millions de possibilités) appelle
maintenant `datamanager_rebalance_step` en boucle avec un budget large (`INT_MAX` — c'est un
appel explicite, pas un tick périodique, on veut converger en un seul appel) jusqu'à
équilibre. `split_datas_nolock` (utilisée en interne par `sort_descending_mthread`, qui a
besoin d'une redistribution **exacte** par quotient, pas d'une convergence incrémentale)
reste inchangée — seule la version publique, sans argument, est réécrite.

Commande console `rebalance [n]` ajoutée pour déclencher immédiatement un pas plutôt que
d'attendre le prochain tour — `n` optionnel (défaut `rebalance_budget`). **Correction par
rapport au plan initial** : classée `server_only = 0` (cosmétique, comme `split`/`regroup`,
pas `1` comme envisagé) — le mécanisme est générique et fonctionne aussi bien sur le stock
local d'un client, exactement comme ses deux voisines. Ajoutée au whitelist privilégié de
l'API HTTP admin (`control_command_privileged`) aux côtés de `split`/`regroup`.

## 5. Nombre de files configurable au démarrage (PR 4, proposition)

Ferme le `@todo Rendre configurable` déjà présent sur `NB_FILE_POSSIBILITY`
(`src/core/datamanager.h`). Un plus grand nombre de files réduit le temps d'écriture par
file (PR 2) et la granularité du rééquilibrage (PR 3). Fixé une seule fois au démarrage
(`--stock-files <n>`), jamais à chaud. Contrainte à préserver : le nombre de files doit
rester ≥ au nombre de forks des clients (le pool analysé y est indexé par `fork_seq` côté
client).

## 6. Une sauvegarde inutile ne s'exécute pas (PR 5, proposition)

**Le principe existe déjà, vérifié dans le code** : `should_autobackup`
(`src/app/etii_server.c`) saute déjà la sauvegarde quand rien n'a changé —
`clientsFileUpdates` n'avance que si un `INST_GET`/`INST_GET_TO_CHECK[_BATCH]`/`INST_ADD` a
réellement touché un pool de stock. Un stock inactif ne déclenche déjà aucune écriture
aujourd'hui.

Le vrai trou, trouvé en traçant les incréments : les quatre artefacts sauvegardés à la même
cadence (`backup`, `backup_analysed`, `best_board_save`, `known_clients_registry_save`)
partagent une **unique** porte, keyed uniquement sur le trafic stock — les acquittements
`INST_POSSIBILITY_ANALYSED[_BATCH]` n'incrémentent rien. Une activité purement pruner peut
laisser `temp_analysed.back` périmé sans le signaler ; symétriquement, une activité
purement stock réécrit les trois autres fichiers pour rien.

Proposition : un compteur de mutations indépendant par artefact, et — en composition avec
PR 2 — un drapeau « modifiée depuis la dernière sauvegarde » **par file de stock**, pour
qu'une file non modifiée ne soit même pas verrouillée en phase 1 de la sauvegarde
progressive, pas seulement écrite à coût nul.

## 7. Hors série, laissé ouvert

Le vrai plafond reste le **volume** : 14 millions de possibilités ≈ 9 Go résident sur une
machine de 16 Go, et rien ne borne le stock à l'exécution (`expand_max_stock` ne borne que
l'expansion de démarrage, pas la croissance ultérieure). À étudier séparément : plafond
global de stock, débordement sur disque, ou sauvegarde par delta plutôt que réécriture
intégrale. `rmnonext` (§1) partage le défaut structurel de verrou global sans sortie bornée
— non couvert par cette série, faute d'avoir été le déclencheur observé.

## 8. Vérification

`make test`, `make test-docker` (Linux/gcc + ASan attrapent ce que macOS masque) et
`make test-integration` pour chaque PR. Reproduction réelle obligatoire (règle du projet :
une fonctionnalité n'est finie qu'exercée dans le vrai binaire — c'est exactement ce qui a
manqué avant ce diagnostic) : rejouer la séquence d'origine
(`--expand-level 9 --expand-max-stock 100000000 --expand-max-levels 10`, puis pruner 4
forks par lots de 100) et vérifier l'absence de `Broken pipe`, `ack=4`,
`batch analysed : possibilité non retirée` et `Error on need work poll` sur une durée
franchement supérieure à un cycle d'autobackup, avec un palier intermédiaire
(`--expand-level 7`) pour distinguer ce qui relève du verrouillage de ce qui relève du
volume.
