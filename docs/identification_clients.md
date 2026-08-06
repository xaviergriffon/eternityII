# Identification des clients — document de conception

**Statut : proposition.** Rien de ce document n'est implémenté. Il sert de base au
découpage en PR et fixe les arbitrages déjà tranchés, pour qu'ils n'aient pas à être
rediscutés à chaque PR.

Objectif : remplacer le PID comme identifiant de client par un modèle capable de
soutenir trois besoins qui vont au-delà du simple affichage lisible :

1. **envoyer une commande à un client précis** (aujourd'hui : diffusion uniquement) ;
2. **des statistiques par client, cumulées et persistantes** — elles survivent à la
   déconnexion d'un client **et** au redémarrage du serveur ;
3. **attribuer les possibilités en cours d'analyse** à leur détenteur, avec un
   **bail à expiration** qui les rend automatiquement quand le client disparaît.

---

## 1. État des lieux

### 1.1 Ce qui tient lieu d'identité aujourd'hui

Toute l'identité d'un client tient dans `control_hello_t`
([src/net/control_protocol.h](../src/net/control_protocol.h)) : `pid`, `nb_forks`,
`mode`. C'est ce que restituent la console `clients`
([src/ui/command_lines.c](../src/ui/command_lines.c)) et
`GET /api/v1/clients` ([src/net/http_codec.c](../src/net/http_codec.c)).

Le PID est insuffisant sur trois plans :

| Faiblesse | Conséquence |
|---|---|
| Non unique entre machines | Deux clients de deux hôtes différents peuvent s'afficher sous le même PID |
| Recyclé par l'OS | Un client relancé peut réapparaître sous un PID déjà vu ; aucun cumul fiable |
| Non parlant | Rien n'indique *quelle* machine du parc décroche |

### 1.2 Deux connexions, aucune corrélation

C'est le point structurant, et il invalide l'idée que l'identification serait une
affaire du seul canal de contrôle :

- la connexion **de travail** (`INST_GET`/`INST_ADD`/…) est ouverte par **chaque fork
  de recherche** et ne déclare **rien** : après le handshake de version elle envoie
  directement ses instructions ([src/app/etii_server.c](../src/app/etii_server.c)) ;
- la connexion **de contrôle** est ouverte par le **processus parent** uniquement, et
  elle est la **seule** à s'annoncer (`INST_CONTROL_HELLO`).

Un client de 16 forks ouvre donc 17 connexions dont **une seule est identifiée**, et le
serveur n'a aucun moyen de rattacher les 16 autres au parent. Tout ce qui se mesure sur
le trafic de travail (possibilités servies, débit réellement consommé, taux d'`ADD`)
n'est aujourd'hui **attribuable à personne**.

### 1.3 Les analyses en cours sont anonymes

Côté serveur, chaque `INST_GET` / `INST_GET_TO_CHECK` / `INST_GET_TO_CHECK_BATCH` fait
`add_possibility_analysed(&…, -1)` : le paramètre `thread` vaut **-1**, aucun
propriétaire n'est enregistré. Il en découle que :

- « que travaille le client X en ce moment ? » est une question sans réponse ;
- la récupération à la déconnexion repose sur `*lastSent`, un état **par connexion** qui
  ne couvre que le **dernier** GET — au-delà, ce qu'un client tenait reste gelé dans
  `in_analyse` jusqu'à un `restock` manuel, qui est **tout-ou-rien** ;
- un client tué brutalement (`kill -9`, coupure réseau, panne machine) gèle sa part du
  stock **indéfiniment**.

### 1.4 Les compteurs sont indexés par slot recyclé

`counters[client->compteur]` et `fileUpdates[client->compteur]` sont indexés par le
**slot du pool `client_t`**, réattribué au client suivant. La seule statistique par
client, le `CTRL_STATS` mis en cache par `control_registry_record_stats`, est un
**instantané**, remis à « pas de stats » à chaque `register`/`unregister` — rien ne
survit à une reconnexion. Aucun cumul n'est possible dans ce modèle.

### 1.5 L'adressage n'existe pas

`control_registry_broadcast_command` est diffusion-seule, et l'indice de session
(`0..MAX_CONTROL_SESSIONS-1`) est un **slot réutilisable**, pas une identité : un
`--to 3` naïf frapperait le client qui occupe le slot au moment de la résolution, pas
celui que l'opérateur avait vu dans la liste.

---

## 2. Arbitrages retenus

| # | Question | Décision |
|---|---|---|
| A | Le cumul de statistiques survit-il au redémarrage du serveur ? | **Oui.** Donc les compteurs par client entrent dans le périmètre de la persistance (nouveau fichier `.back`), **et** la clé de cumul doit être stable au-delà de la vie d'un processus client ⇒ `machine_uid` persisté côté client est **requis**, pas optionnel |
| B | Bail à expiration sur les possibilités en analyse ? | **Oui**, envisagé et retenu comme cible. Un client disparu rend son stock tout seul |
| C | Où stocker le propriétaire d'une possibilité ? | **Table latérale côté serveur**, adossée à l'index `analysed_index` existant — **jamais** dans `possibility_packet` |
| D | Un ou deux registres serveur ? | **Deux** : `control_registry` (sessions vivantes, pilotage) et un nouveau registre de **clients connus** (cumul, persistant) |
| E | Confiance dans ce que déclare le client | Déclaratif par nature. L'IP du pair, dérivée par le serveur, est conservée comme champ **non falsifiable** affiché à côté |

---

## 3. Modèle d'identité

Quatre notions distinctes, à **ne pas fusionner** en un seul champ — chacune a une durée
de vie et un rôle différents.

| Notion | Portée / durée de vie | Rôle | Qui la produit |
|---|---|---|---|
| `machine_uid` | **Persistante** (fichier local), survit aux redémarrages | Clé de **cumul** des statistiques | Client, tirée au premier lancement puis relue |
| `client_uid` | Une **exécution** d'un processus parent | Clé d'**identité de session** : pilotage, propriété des baux | Client, nonce 128 bits tiré au démarrage |
| `fork_seq` | Un fork dans son parent (`0..N-1`) | Rattache une connexion **de travail** à son parent | Client |
| `label` | Déclaratif, arbitraire | **Affichage seul** — jamais une clé | Opérateur (`--name`), défaut = hostname |
| `session_no` | Vie du serveur, **jamais réattribué** | Ergonomie console (`--to 3`) | Serveur, compteur monotone |

Règles qui découlent du tableau :

- **`client_uid` tranche toute ambiguïté.** Deux clients peuvent porter le même `label`
  et c'est acceptable : la clé n'est jamais le nom.
- **`(client_uid, fork_seq)`** identifie une connexion de travail. L'agrégation par
  client est un simple regroupement sur `client_uid`.
- **`machine_uid` ≠ `client_uid`.** Confondre les deux rend le cumul persisté inutile :
  avec un nonce par exécution, aucun client ne se reconnaît après son propre
  redémarrage, et le fichier de cumul n'accumule que des lignes mortes.
- **`session_no` n'est pas un slot.** Il est résolu vers `client_uid` **avant**
  exécution ; si le titulaire a changé, la commande est **refusée**, pas redirigée.

Affichage cible :

```
#3  jetson-1 (192.168.1.42)  pid 4711  pruner_gpu  8 forks  vu il y a 2 s
```

### 3.1 Fichier d'identité machine

- Emplacement : fichier local côté **client** (chemin à fixer en PR — surchargeable par
  option/variable d'environnement, car un déploiement conteneurisé a besoin de le monter
  en volume, sinon chaque redémarrage de conteneur regénère un `machine_uid` et le cumul
  se fragmente).
- Contenu : un nonce, rien d'autre.
- Absent ou illisible ⇒ **tirer et écrire**. Échec d'écriture ⇒ **avertir et continuer**
  avec un uid volatil : l'impossibilité de cumuler ne doit jamais empêcher de chercher.

---

## 4. Impacts par domaine

### 4.1 Protocole (bump `VERSION`, actuellement 11)

Le handshake est un **exact-match**, donc toute modification du fil impose
`VERSION 12`. À faire **une seule fois**, en portant l'ensemble des champs :

- **Nouveau hello sur la connexion de travail** — c'est l'ajout de fond : sans lui,
  ni attribution des analyses en cours, ni statistiques mesurées par le serveur. Il
  porte `machine_uid`, `client_uid`, `fork_seq`, `label`, `mode`.
- **`control_hello_t` étendu** des mêmes champs (`CONTROL_HELLO_WIRE_SIZE` à ajuster,
  ainsi que `control_hello_encode`/`decode` et leurs tests).
- Champs de largeur fixe et explicites, comme le reste de `control_protocol.h` (jamais
  un struct brut sur le fil). `label` : longueur préfixée et **bornée**, jamais lue
  au-delà de la longueur annoncée.

> Rappel à respecter en PR : ne **jamais** rejouer la suite de tests vN+1 contre un
> binaire vN (blocage dans `recv_all`), et committer avant tout échange de fichiers.

### 4.2 Serveur — registre de clients connus (nouveau)

Distinct de `control_registry`, avec des rôles qui ne se recouvrent pas :

| | `control_registry` (existant) | Registre de clients (nouveau) |
|---|---|---|
| Indexé par | slot de session (réutilisé) | `machine_uid` (cumul) / `client_uid` (session) |
| Durée de vie | la session TCP | la vie du serveur, **+ persistée** |
| Contenu | hello, file de commandes, dernier `CTRL_STATS` | totaux cumulés, première/dernière vue, statut connecté/déconnecté |
| Rôle | **piloter** | **mesurer** |

Ne pas chercher à n'en faire qu'un : le premier doit être vidé à la déconnexion, le
second ne doit précisément pas l'être.

À décider en PR : la **borne** de ce registre et la politique d'éviction (un parc qui
tourne pendant des mois accumule des entrées de machines définitivement parties).

### 4.3 Serveur — attribution et bail des analyses en cours

- L'emplacement du propriétaire est une **table latérale**, adossée à l'index déjà
  présent : `analysed_index` / `hash_possibility_key`
  ([src/core/datamanager.c](../src/core/datamanager.c)) indexe **déjà** les entrées en
  analyse pour `remove_possibility_analysed`. Y adosser `{owner_uid, lease_deadline}`
  donne l'attribution **et** l'expiration sans toucher au format de
  `possibility_packet` ni à celui des backups.
- **Pourquoi pas dans `possibility_packet`** : cette structure est sur le fil *et* dans
  les backups (`eternityII-in_analyse.back`), et elle comporte du **padding caché**
  malgré `packed` — l'élargir touche le format de persistance, le fil, et toutes les
  fixtures de test d'un coup.
- **Pourquoi pas le paramètre `thread`** de `add_possibility_analysed(p, thread)` : il
  vaut `-1` côté serveur et l'indice de thread côté client. Sémantique déjà surchargée,
  ne pas y empiler un troisième sens.
- Le balayage d'expiration doit être **borné et périodique** — le pas de
  `check_server_step` (toutes les 10 s, où vit déjà l'autobackup) est le candidat
  naturel ; surtout pas un parcours dans un chemin chaud.
- L'expiration doit être **idempotente** vis-à-vis de `remove_possibility_analysed` :
  un acquittement qui arrive juste après l'expiration ne doit ni doubler la possibilité
  dans le stock, ni échouer bruyamment. C'est le point délicat de cette partie.
- Durée du bail : à dimensionner **au-dessus** du temps qu'un client peut légitimement
  passer sur un lot (un pruner à gros `prunerBatch` est le cas majorant), et à rendre
  configurable. Un bail trop court se traduit par du travail dupliqué, pas par une
  erreur visible — donc à choisir prudemment large.

### 4.4 Console

- `clients` : afficher `session_no`, `label`, IP, PID, mode, forks, dernière activité.
- `clientsCmd` : ajouter un **adressage** (`--to <session_no|label|uid>`), la diffusion
  restant le comportement par défaut sans option. Passe par les **mêmes** listes
  blanches (`control_command_allowed`) : cibler un client n'élargit jamais le jeu de
  commandes autorisées.
- Éventuellement une commande de consultation du cumul et des baux en cours.
- Toute nouvelle commande ⇒ entrée dans la table `commands[]` (aide, usage, alias).

### 4.5 API HTTP

- `GET /api/v1/clients` : champs **additifs** (`session_no`, `label`, `machine_uid`,
  `client_uid`, `ip`, totaux cumulés) — non cassant pour un consommateur existant.
- Nouvelles routes envisageables : cumul par machine, baux en cours. Une commande
  ciblée passe par `POST /api/v1/command` (whitelist standard inchangée).

### 4.6 CLI

- `--name <label>` : option position-indépendante, retirée d'`argv` avant l'analyse
  positionnelle, comme `--expand-level`/`--http-port`.
- Option/variable pour le chemin du fichier `machine_uid`.
- **Toute nouvelle option ⇒ entrée dans `cli_topics[]`**
  ([src/app/app_runtime.c](../src/app/app_runtime.c)).

### 4.7 Persistance

Un nouveau fichier de sauvegarde pour le cumul, aux côtés de
`eternityII-best_board.back`, accroché aux **mêmes** points d'appel (autobackup,
`--stop-on-solution`, `restore`). Le format doit être **tolérant en avant** : un cumul
illisible ou d'une version antérieure se traduit par « on repart de zéro sur le
cumul », **jamais** par un refus de démarrer — c'est une donnée d'observation, pas de
l'état de recherche.

Les **baux ne sont pas persistés** : au redémarrage, `in_analyse` est restauré sans
propriétaire (donc réputé libre, comportement actuel). Persister des baux dont les
titulaires sont de toute façon déconnectés n'aurait aucun sens.

---

## 5. Découpage en PR proposé

Ordonné pour que chaque PR soit livrable et mesurable seule, et pour ne payer le bump de
protocole **qu'une fois**.

| PR | Contenu | Bump `VERSION` | Dépend de |
|---|---|---|---|
| **1** | **IP du pair** : `accept()` avec `sockaddr`, conservation dans `client_t`, affichage console + `GET /api/v1/clients` | non | — |
| **2** | **Identité déclarée** : `machine_uid`/`client_uid`/`fork_seq`/`label`, `--name`, fichier d'identité machine, hello de travail + `control_hello_t` étendu, `session_no` serveur | **oui (12)** | 1 |
| **3** | **Adressage des commandes** : `clientsCmd --to …`, résolution `session_no` → `client_uid` avec refus si le titulaire a changé | non | 2 |
| **4** | **Registre de clients connus** : cumul en mémoire, statut connecté/déconnecté, exposition console + HTTP | non | 2 |
| **5** | **Persistance du cumul** : nouveau `.back`, branché sur les points d'appel existants, lecture tolérante | non | 4 |
| **6** | **Attribution des analyses en cours** : `{owner_uid}` dans la table latérale, consultation « que travaille X » | non | 2 |
| **7** | **Bail à expiration** : `lease_deadline`, balayage borné dans `check_server_step`, remise en stock idempotente, durée configurable | non | 6 |

Notes de séquencement :

- La **PR 1** est autonome et sans bump : elle règle déjà l'essentiel du besoin de
  lisibilité sur un parc de machines distinctes, et peut partir immédiatement.
- La **PR 2** est le pivot : tout le reste en dépend, et c'est la seule qui touche le
  fil. Ne pas la fractionner, sous peine de deux bumps.
- Les **PR 6 et 7** sont séparées volontairement : l'attribution est une lecture
  (observable, sans risque), l'expiration est une **écriture sur le stock** — la partie
  qui peut dupliquer ou perdre du travail si elle est fausse. Elle doit pouvoir être
  revue et testée seule.

## 6. Tests attendus

Conformément à la règle du projet (un test par comportement ajouté et par bogue
corrigé), en privilégiant l'extraction de fonctions pures :

- encodage/décodage des hellos étendus, y compris **payload tronqué** et `label` de
  longueur maximale ;
- unicité et format du `client_uid` ; lecture/écriture du fichier `machine_uid`, dont le
  chemin **non inscriptible** (`fork_assert.h`, avec `SKIP_IF_ROOT()` si le test repose
  sur un refus d'accès du système de fichiers) ;
- résolution `session_no` → `client_uid` : cas nominal, titulaire changé, inconnu ;
- cumul : addition, survie à un `unregister`, aller-retour sauvegarde/restauration,
  fichier tronqué ou corrompu ;
- bail : expiration pure (fonction sans horloge réelle, échéance passée en paramètre),
  **acquittement concurrent d'une expiration** (idempotence), non-régression sur le
  chemin `lastSent` existant ;
- intégration : un client identifié apparaît dans `clients`, reçoit une commande ciblée,
  et son stock est rendu après disparition brutale.

## 7. Points laissés ouverts

- Chemin par défaut du fichier `machine_uid`, et comportement en conteneur.
- Borne et politique d'éviction du registre de clients connus.
- Durée par défaut du bail, et si elle doit dépendre du mode (recherche vs pruner à gros
  lot).
- Faut-il conserver le PID à l'affichage une fois le `label` en place ? (avis : oui, il
  reste le seul moyen de corréler avec `ps`/`top` sur la machine concernée).
