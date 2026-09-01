# Dispatch local des possibilités entre les forks d'un client

**Statut : en cours d'implémentation (2/6 PR livrées).** Ce document décrit une **cible** ;
tout ce qui n'est pas explicitement marqué « livré » ci-dessous n'est pas encore le
comportement du code.

Aujourd'hui, chaque fork d'un client demande **sa** possibilité au serveur et l'étudie seul,
sans borne de durée. Cette proposition décrit un workflow où le client demande le travail
**une fois**, le décompose dès le début de la première étude, et le dispatche à ses propres
forks via des files locales portées par le processus parent — pour raccourcir la durée
d'étude d'une racine et rendre plus vite au serveur ce qui stagne côté client.

---

## 1. Constat chiffré

### 1.1 Un fork étudie une racine, seule, sans borne

En mode recherche, la taille de lot demandée au serveur vaut **1**
([src/app/etii_client.c](../../src/app/etii_client.c) `feed_one_thread`,
`pruner_mode ? pruner_batch_size : 1`). Le fork reçoit donc une racine, et
`autosearch_step` la confie à `search_packet_backtracking`
([src/core/etii_search.c](../../src/core/etii_search.c)). Le seul retour **nominal** de ce
backtracking est `BT_CORE_EXHAUSTED` : « sous-arbre entièrement exploré, prouvé mort ».

Autrement dit : **un fork de recherche ne rend pas une possibilité quand il a fini — finir
signifie qu'il n'y a plus rien à rendre.** Tout ce qui sort du fork avant cela est de la
délégation. Et la durée avant `BT_CORE_EXHAUSTED` n'est bornée par rien : elle dépend
entièrement du niveau et de la difficulté de la racine tirée.

### 1.2 Le travail non exploré est un stock *implicite*, invisible

Il n'y a plus de file explicite de possibilités en attente dans un thread de recherche. Le
stock est **implicite**, reconstitué à la demande depuis la pile de décisions par
`bt_count_pending` ([src/core/etii_search.c](../../src/core/etii_search.c)) : tout candidat
frère non encore essayé, à tout niveau de la pile, dont la pièce n'est pas déjà posée.

Ce stock n'est visible de personne — ni des frères du fork, ni du parent, ni du serveur —
sauf par le compteur agrégé `lastfilesize[]` remonté en statistique.

### 1.3 Il n'est cédé que rarement, et cher

`bt_delegate_if_needed` n'est consulté qu'une fois tous les `DELEGATE_CHECK_INTERVAL_NODES`
= **1 000 000 nœuds**, et n'agit que si `DELEGATE_MIN_INTERVAL_MS` = **500 ms** se sont
écoulées depuis la dernière cession
([src/core/core_static_variables.h](../../src/core/core_static_variables.h)). Le quota est
donné par `bt_delegation_quota` :

- `pending > max_stock_by_thread` (défaut **300**) → céder `max_stock_by_thread` ;
- sinon, céder seulement si le serveur a faim (sonde v8 `INST_NEED_WORK`), et au plus la
  moitié du stock implicite.

La cession part **au serveur**, et `put_to_server`
([src/core/datamanager.c](../../src/core/datamanager.c)) est une boucle **paquet par
paquet** :

```c
send_instruction(socket_id, INST_ADD);
send_all(socket_id, possibility, sizeof(struct possibility_packet));
int8_t ack = recv_instruction(socket_id);   // attend INST_CONSIDERED
```

soit **un aller-retour TCP synchrone par possibilité**, jusqu'à 300 d'affilée, exécutés par
**le thread de recherche lui-même**, sous `socket_mutex` — donc en bloquant aussi le thread
d'alimentation du même fork. `INST_ADD` est d'ailleurs la dernière instruction du protocole
à ne pas être cadrée par lot, alors que `INST_GET` (v7), `INST_GET_TO_CHECK_BATCH` et
`INST_POSSIBILITY_ANALYSED_BATCH` le sont (cf. §9).

### 1.4 Et il est cédé par le mauvais bout

`bt_materialize_pending` parcourt la pile **du plus profond vers le moins profond** :

```c
for (i = top; i >= 0 && count < max_out; i--) {
```

C'est délibéré et correct **pour la délégation au serveur** : on cède les sous-arbres les
moins chers et on garde le haut de l'arbre en local. Mais c'est exactement le mauvais bout
quand l'objectif est de **raccourcir l'étude d'UNE racine** : les frères les moins profonds
sont les gros sous-arbres, ceux dont la parallélisation paie.

### 1.5 Deux forks d'un même client ne se voient pas

Les forks sont des **processus séparés** (`fork()`,
[src/app/fork_orchestrator.c](../../src/app/fork_orchestrator.c)). Après le fork, chaque
pool `datamanager` (`file_possibility`, `file_possibility_checked`,
`file_possibility_analysed`) est une copie COW privée qui diverge immédiatement. La seule
chose réellement partagée est la map de lookup, en lecture seule (cf.
[architecture.md](../architecture.md#map-de-lookup-partagée-entre-les-processus-de-recherche)).

**Le seul point de rendez-vous entre les forks d'un même client est donc le serveur** — au
prix d'un aller-retour réseau et de la contention sur les verrous du stock serveur.

Les files locales du parent, elles, existent déjà et sont **vides** :
`datamanager_configure_stock_files` est appelée **inconditionnellement dans tous les rôles**
([src/app/main.c](../../src/app/main.c)), client compris. C'est le point d'appui de cette
proposition.

### 1.6 Les deux remèdes existants sont tous les deux médiés par le serveur

Le problème de famine n'est pas neuf, et deux mécanismes l'attaquent déjà — mais tous deux
**passent par le serveur** :

- la **sonde de faim v8** (`INST_NEED_WORK`) et la délégation anticipée qu'elle pilote
  ([echanges_client_serveur.md §Gestion de charge](../echanges_client_serveur.md#gestion-de-charge)),
  dont la motivation d'origine décrit exactement le symptôme visé ici : « le paquet genèse
  part chez un premier thread dont le stock implicite reste longtemps sous
  `max_stock_by_thread` — il ne délègue rien, pendant que tous les autres clients reçoivent
  K = 0 » ;
- l'**expansion au démarrage** (`--expand-level`), qui prévient la même famine *à la source*,
  côté serveur.

Aucun mécanisme n'existe **à l'intérieur** d'un client. C'est le vide que ce document propose
de combler.

---

## 2. Workflow cible

Le processus parent du client devient le **courtier** de ses forks.

```
                          ┌──────────────────────────── Client ────────────────────────────┐
                          │                                                                │
   Serveur                │   Parent (courtier)                                            │
  ┌────────┐   1 GET      │  ┌──────────────────────────┐                                  │
  │ stock  │◄────────────►│  │ files locales            │   3. dispatch   ┌────────┐       │
  │ global │   ADD/ack    │  │ (file_possibility[…],    ├────────────────►│ fork B │       │
  └────────┘   INST_NEED  │  │  déjà allouées, vides)   │  etii_fork.<pid>├────────┤       │
       ▲       _WORK      │  │                          │◄────────────────┤ fork C │       │
       │                  │  │ compteur de terminaison  │                 └────────┘       │
       │                  │  │ par racine               │                                  │
       │                  │  └──────────▲───────────────┘                                  │
       │                  │             │ 2. frères les moins profonds                     │
       │                  │             │    (etii_main.<pid>)                             │
       │                  │        ┌────┴───┐                                              │
       │                  │        │ fork A │  étudie la racine                            │
       │                  │        └────────┘                                              │
       │                  └────────────────────────────────────────────────────────────────┘
       │
       └── 4. péremption / faim annoncée : ce que personne ne réclame repart au serveur
```

1. **Une seule demande par client.** Le parent porte l'unique connexion de travail et
   demande une racine au serveur. `client_identity_t.fork_seq = -1` désigne déjà « le
   processus parent » ([src/net/client_identity.h](../../src/net/client_identity.h)) : la
   convention existe.
2. **Partage dès le début de l'étude.** Le fork qui reçoit la racine matérialise tôt — pas
   au bout de 500 ms — ses frères **les moins profonds** et les renvoie au parent sur
   `etii_main.<pid>`.
3. **Dispatch local.** Le parent les range dans ses propres pools (`put_to_local`) et les
   distribue aux forks au repos sur `etii_fork.<pid>`. Aucun aller-retour réseau, aucune
   contention sur les verrous du serveur.
4. **Ce qui stagne repart.** Ce qu'aucun fork ne réclame dans un délai borné, ou tout le
   tampon dès que la sonde de faim annonce que le serveur en veut, est `ADD`é au serveur par
   le parent.

Le canal IPC est déjà dimensionné pour cela : `ipc_max_datagram()`
([src/net/local_socket.c](../../src/net/local_socket.c)) vaut **4002 octets** et
`tests/net/test_local_socket.c` assère explicitement
`>= 1 + sizeof(struct possibility_packet)` (576 octets sur le puzzle 256). `IPC_MSG_BEST_BOARD`
transporte déjà un paquet entier fils → parent.

---

## 3. Arbitrages tranchés

| # | Arbitrage | Raison |
|---|---|---|
| **A1** | **Courtier dans le processus parent, via l'IPC AF_UNIX existant** — ni mémoire partagée POSIX, ni bascule des forks en threads | Les deux sockets existent et sont déjà dimensionnées pour un `possibility_packet` (§2), les pools `datamanager` du parent sont déjà alloués et vides (§1.5) : `put_to_local`/`scroll_from_local`, le plafond RAM et `stockDistribution` viennent gratuitement. Et **aucun verrou process-shared à traverser un `fork()`** — cf. les invariants de sûreté au fork d'`AGENTS.md`, dont chacun a été violé une fois en production. Les threads, eux, abandonneraient l'isolation par process et le partage COW de la map |
| **A2** | **Une seule connexion de travail par client**, portée par le parent | C'est la formulation même du besoin (« une demande pour 1 client »). Conséquence lourde : cinq mécanismes serveur comptent aujourd'hui les *connexions* pour compter les *forks* — cf. §4 |
| **A3** | La file locale est un **tampon de dispatch transitoire, pas un stock** : bornée en volume, avec poussée au serveur sur péremption | Elle n'est **pas durable** : rien n'y survit à un crash du client, alors que le stock serveur est sauvegardé (`consistent_backup`). En faire un stock déplacerait de la donnée non sauvegardée hors du seul endroit qui la sauvegarde |
| **A4** | **Invariant d'acquittement** : une racine n'est acquittée au serveur que lorsque toute sa descendance dispatchée localement est terminée ou a été ré-`ADD`ée au serveur. Implémenté par un **compteur de terminaison par racine** dans le parent, jamais par inspection de filiation | Préserve l'invariant actuel : « acquitté ⇒ mort prouvé, ou déjà remis en stock ». Le compteur est en O(1) par message ; comparer les plateaux (`is_origin_of`) serait en O(stock). **Mode dégradé correct par défaut** : si un fork meurt, le compteur n'atteint jamais zéro, le parent n'acquitte simplement pas, et le bail existant reprend la main (`datamanager_reclaim_expired_leases` puis `datamanager_purge_descendants_of`). Aucun nouveau mode de défaillance n'est introduit |
| **A5** | Phase 1 : **forks de recherche uniquement**. Les forks pruner gardent leur connexion propre | Le pruner a son propre pool (non vérifié), son propre protocole par lot (`INST_GET_TO_CHECK_BATCH`, 100 paquets) et son propre dosage (`--pruner-forks`, `--auto-roles`). Les intégrer doublerait la surface de la première étape. **Conséquence assumée** : un client mixte garde 1 connexion de recherche + 1 par fork pruner, pas strictement 1 connexion |
| **A6** | Dispatch **local** : frères **les moins profonds d'abord** (nouveau mode de `bt_materialize_pending`). Délégation **serveur** : plus profonds d'abord, inchangée | En local on veut paralléliser les **gros** sous-arbres, c'est là qu'est la latence ; vers le serveur on veut céder du travail bon marché à bas coût. **À confirmer par la mesure** (§6) — la direction du tri est exactement le genre d'arbitrage que ce dépôt a déjà tranché par mesure et non par intuition (cf. le départage MRV par nombre de côtés contraints, [elagage_recherche.md §4.12](elagage_recherche.md)) |
| **A7** | Comportement **opt-in** derrière une option, défaut inactif | Pour que l'A/B tienne dans **un seul binaire**, condition d'une mesure appariée honnête. Précédents : `--auto-roles` (défaut off), `pruner_dfs_budget` (opt-in) |

---

## 4. Régressions à traiter — ce que A2 casse

Passer de N connexions de travail à 1 par client casse **cinq mécanismes serveur qui comptent
les connexions pour compter les forks**, plus trois limites du canal IPC. Chacune doit être
traitée par la PR qui introduit A2, sinon l'implémentation les découvrira en production.

| # | Mécanisme | Ce qui casse | Correctif proposé |
|---|---|---|---|
| **R1** | `compute_server_hunger(stock, active_clients)`, alimenté par `get_active_threads()` (compte les `socket_id != -1`) — [src/app/etii_server.c](../../src/app/etii_server.c) | La cible `active_clients * SERVER_HUNGER_PER_CLIENT` est **divisée par le nombre de forks**. La sonde v8 cesse d'annoncer la faim réelle, et la délégation anticipée (`bt_delegation_quota`) meurt avec elle — on casserait précisément le mécanisme de §1.6 qu'on cherche à compléter | `control_hello_t.nb_forks` est **déjà sur le fil** ([src/net/control_protocol.h](../../src/net/control_protocol.h), lu par le serveur au hello de contrôle) : pondérer par lui plutôt que par le nombre de connexions |
| **R2** | `client_work_fork_roles` → console `clientsWork` ([src/ui/command_lines.c](../../src/ui/command_lines.c)) | N'énumère plus qu'une connexion : la vue « rôle de chaque fork » disparaît | Reconstruire la vue depuis le canal de contrôle (`nb_forks` + dosage désiré déjà mémorisé par `machine_uid`), ou assumer et **documenter** la dégradation |
| **R3** | `server_analysed_file_hint` = `client->compteur % nb_file_possibility` | Une connexion par client ⇒ indice **constant** par client ⇒ tout le pool analysé d'un client se concentre sur **une** file, annulant le round-robin de PR8 (*Server load management*) et ramenant l'acquittement au comportement qui avait motivé cette PR | Dériver l'indice d'un compteur **par requête** plutôt que par connexion |
| **R4** | `owner_client_alive` = session de contrôle **OU** connexion de travail ouverte | Les deux signaux se rejoignent sur le même process : c'est le cas dégénéré à un seul signal que [bail_expire_racines_en_stock.md](../investigations/bail_expire_racines_en_stock.md) a documenté (28,5 % d'un stock de production devenu racine de lui-même) | Le résultat est en fait **plus sûr** — le parent survit à ses forks, alors que le canal de contrôle se fermait *avant* que les forks aient fini de vider. Mais le raisonnement doit être **réécrit et re-testé**, pas hérité en silence |
| **R5** | Métriques de besoin ventilées par rôle, via `identity.mode` de la connexion de travail | Une connexion parent porte **un seul** `mode` | Même source que R2 : le canal de contrôle |
| **R6** | Le parent devient le goulot du travail | Ordre de grandeur à mesurer, pas à supposer : 16 forks × jusqu'à 300 paquets / 500 ms × 577 o ≈ **5,5 Mo/s** sur AF_UNIX, chez un process qui porte déjà la console (ncurses) et le routage des logs de tous ses forks | Mesure §6 ; borne du tampon (A3) ; lots plutôt que datagrammes unitaires |
| **R7** ✅ *(PR1)* | `fork_udp` ([src/app/app_runtime.c](../../src/app/app_runtime.c)) : tampon de **100 octets**, canal **texte seul**, et `value[numBytes]` écrit à l'indice 100 sur une lecture pleine — **débordement d'un octet, préexistant** | Un paquet de 576 o ne peut pas passer parent → fils, même si les tampons de la socket le permettraient déjà | Messages typés (comme le sens fils → parent) + tampon dimensionné par `ipc_max_datagram()`. **Le débordement est à corriger de toute façon**, indépendamment de cette proposition |
| **R8** ✅ *(PR1)* | `send_command_to_childs` ([src/net/local_socket.c](../../src/net/local_socket.c)) mesure la charge utile par `strlen()` | Impossible d'envoyer un binaire (un paquet contient des octets nuls) | Variante portant une longueur explicite |

---

## 5. Ce que ce workflow ne résout pas

À écrire noir sur blanc pour que la mesure ne soit pas lue de travers :

- **Aucun gain d'élagage.** Le nombre de nœuds à explorer pour prouver un sous-arbre mort est
  inchangé. Cette proposition déplace de la **latence**, elle ne réduit pas le travail. Le
  banc de coût de réfutation (`make bench-refutation`) ne doit donc **rien** montrer.
- **Aucun effet sur un client mono-fork.** Il n'y a personne à qui dispatcher. Le seul effet
  résiduel serait la poussée anticipée vers le serveur (§2, étape 4).
- **Aucune durabilité ajoutée.** Le tampon local n'est pas sauvegardé (A3) ; c'est A4 qui
  garantit qu'on ne perd rien, en n'acquittant pas.
- **Pas un remplacement de `--expand-level`.** L'expansion serveur fabrique du stock *avant*
  qu'il manque ; ce workflow redistribue du stock *déjà attribué*. Les deux se composent.

---

## 6. Protocole de mesure

Une seule de ces métriques est à instrumenter ; les autres existent.

| Métrique | Source | Attendu |
|---|---|---|
| **Temps de séjour d'une racine** (GET → acquittement), en distribution et pas seulement en moyenne | **À instrumenter** : compteur dans le parent, exposé par `statistic` et `GET /api/v1/stats` | **C'est l'objectif.** Doit baisser |
| Débit cumulé (coups/s) | `statistic`, `GET /api/v1/stats` | Ne doit **pas** régresser |
| Débit ADD vers le serveur | `stock_adds_*_rate` ([src/core/stock_rate.c](../../src/core/stock_rate.c)), fenêtres 1 min / 1 h / 1 j | « Rendre plus vite au serveur » se lit **directement** ici |
| Famine serveur (réponses `K = 0`) | `server_search_starved` | Doit baisser |
| Taille du stock serveur dans le temps | `statistic` | Ne doit pas s'effondrer (un client qui garde tout en local viderait le serveur sans le dire) |
| Coût de réfutation | `make bench-refutation` | Doit être **plat** (§5) |

**Méthode : paires alternées, moyenne géométrique.** C'est le protocole déjà employé dans ce
dépôt, précisément parce qu'une machine dérive pendant la campagne (−14,6 % de dérive
thermique mesurée dans [architecture.md](../architecture.md), qui pénaliserait
systématiquement le binaire passant en second) — voir aussi
[elagage_recherche.md](elagage_recherche.md). Quatre paires minimum, en **alternant l'ordre**
des deux configurations.

**Échelles** : correction fonctionnelle sur le puzzle 16 pièces (`make test-integration`,
scénarios client/serveur bout en bout) ; mesures sur le puzzle 256 avec `--expand-level`
pour amorcer un stock réaliste, et au moins deux nombres de forks (1 — pour vérifier §5 — et
8 ou 16).

---

## 7. Découpage en PR

**Resséquencé après PR1** : le découpage initial faisait de la connexion de travail du
parent la 4ᵉ étape, alors que le courtier de la 2ᵉ en a besoin comme **sortie serveur** —
et réciproquement, le passage à la connexion *unique* est impossible tant que le courtier
ne sait pas alimenter les forks, qui perdraient toute source de travail. La dépendance est
circulaire ; elle se dénoue en donnant sa connexion au parent **en même temps** que son
premier usage (le relais sortant), et en ne retirant celles des forks qu'une fois le
courtier capable de les nourrir.

| PR | Contenu | Mesurable seule |
|---|---|---|
| **1** ✅ **livrée** | **IPC typé bidirectionnel** : le sens parent → fils est cadré comme l'autre (octet de type `IPC_MSG_COMMAND`), `send_typed_to_childs` prend la longueur en paramètre (R8), `fork_udp` dimensionne son tampon sur `ipc_max_datagram()` et délègue le découpage à `ipc_child_frame_decode`, fonction pure qui vérifie la place du terminateur (**corrige le débordement d'un octet**, R7). **Écart assumé avec le plan initial** : les types `IPC_MSG_WORK_*` ne sont **pas** introduits ici — une constante sans émetteur ni récepteur ne se teste pas et rote ; ils arrivent avec leur usage. Le transport est générique et **testé sur une charge utile binaire réelle** (un `possibility_packet` complet, octets nuls compris). Effet de bord : une commande de plus de 99 caractères n'est plus tronquée en silence | non (préparatoire) |
| **2** ✅ **livrée** | **Le parent ouvre une connexion de travail et relaie les ADD de ses forks** (`--local-dispatch`, défaut inactif). Un fork offre son lot au parent (`IPC_MSG_WORK_OFFER`) au lieu d'`ADD`er lui-même ; le parent l'empile et le pousse au serveur depuis son propre socket (`fork_seq=-1`). Acquittement différé (A4) par numéro d'offre : le courtier renvoie le plus grand `seq` rendu durable (`IPC_MSG_WORK_SETTLED`), et `send_possibility_analysed` ne fait rien tant qu'il reste des offres non réglées. Contrôle de flux par fenêtre : au-delà de `WORK_BROKER_OFFER_WINDOW` offres en vol, le fork retombe sur l'envoi direct — le tampon du parent est donc borné **par construction**, ce qu'un datagramme UDP (aucun refus à faire remonter) impose de toute façon. **Écart assumé avec A1** : le courtier utilise une file privée, pas les pools `datamanager` du parent — ceux-ci sont ce que `backup`/`restore`/`stockDistribution` manipulent, et y verser un tampon de transit ferait écrire sur disque, sous le nom de « stock », de la donnée qui n'en est pas ; la file privée porte en plus l'origine (`slot`, `seq`) de chaque paquet, dont le règlement a besoin et qu'un pool ne transporte pas. Deux crochets injectés dans `datamanager` (offre, verrou d'acquittement) préservent la règle « `core/` ne dépend jamais d'`app/` ». Vérifié bout-en-bout : 276 possibilités réellement relayées sur un client 3 forks contre un serveur `--expand-level 4` | oui |
| **3** | **Redistribution aux forks au repos** : le parent sert ses forks depuis son tampon (`IPC_MSG_WORK_GRANT`), compteurs de terminaison **par racine** (A4), réinjection du travail en vol à la mort d'un fork, borne et péremption du tampon (A3) | oui |
| **4** | **Connexion de travail unique** (A2) : les forks abandonnent la leur, avec les correctifs R1, R3, R4, R5 dans la même PR — ils ne sont pas séparables de A2 | oui |
| **5** | **Partage à la racine côté fork** : mode « moins profond d'abord » de `bt_materialize_pending` (A6), déclenchement au **début** de l'étude au lieu d'attendre 500 ms / 1 M nœuds | oui |
| **6** | Campagne de mesure (§6), puis bascule du défaut — **ou abandon motivé, consigné dans ce même document** | — |

## 8. Points laissés ouverts

- **Intégration des forks pruner** (A5 les exclut de la phase 1). Leur protocole par lot et
  leur pool distinct demandent un multiplexage que la première étape n'aborde pas.
- **Que devient `max_stock_by_thread`** quand le stock implicite a une destination locale
  bon marché ? Le seuil de 300 a été calibré contre un coût d'aller-retour TCP ; en local ce
  coût disparaît, donc le seuil n'a plus la même justification. À re-calibrer, pas à
  transposer.
- **Borne et politique de péremption du tampon local** (A3) : en nombre de paquets, en
  octets, en âge ? Et faut-il pousser au serveur par ancienneté (FIFO, comme
  `scroll_fifo` le fait pour le débordement disque) ou par profondeur ?
- **Où placer le compteur de terminaison** (A4) : côté parent uniquement, ou faut-il que le
  fork sache à quelle racine se rattache le travail qu'il reçoit, pour le déclarer lui-même ?
- **Comportement au `stopForks` / `configApply`** : le tampon local doit-il être vidé vers le
  serveur avant le re-fork, ou survivre au cycle ? (Le parent survit, donc les deux sont
  possibles.)

---

## 9. Hors série : cadrer `INST_ADD` par lot

Indépendant de cette proposition, mesurable seul, et rendu **plus rentable** par elle.

`put_to_server` fait aujourd'hui un aller-retour TCP **par possibilité** (§1.3), jusqu'à 300
d'affilée depuis le thread de recherche. `INST_ADD` est la dernière instruction du protocole
à ne pas être cadrée par lot — `INST_GET` l'est depuis v7, `INST_GET_TO_CHECK_BATCH` et
`INST_POSSIBILITY_ANALYSED_BATCH` le sont aussi.

Avec le parent comme **unique** émetteur d'ADD (A2), le gain grossit et l'implémentation se
simplifie : un seul émetteur, un seul socket à cadrer.

Coût : bump de `VERSION` 13 → 14, la poignée de main étant à égalité exacte
([src/app/etii_server.c](../../src/app/etii_server.c)).

**À proposer comme PR séparée, préalable ou parallèle — jamais comme partie de ce workflow**,
pour que sa mesure ne se mélange pas à celle du dispatch local.

---

## Voir aussi

- [echanges_client_serveur.md](../echanges_client_serveur.md) — protocole de travail, canal de
  contrôle, gestion de charge, sonde de faim v8, baux sur les analyses en cours.
- [autosearch_step.md](../autosearch_step.md) — §1.5, le pipeline de délégation actuel.
- [architecture.md](../architecture.md) — modèle de processus, IPC parent ↔ enfants, protocole
  de mesure par paires alternées.
- [bail_expire_racines_en_stock.md](../investigations/bail_expire_racines_en_stock.md) — pourquoi
  la vivacité d'un client se juge sur deux signaux (cf. R4).
