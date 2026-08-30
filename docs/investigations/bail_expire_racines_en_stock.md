# Bail expiré : un parent rendu au stock devient la racine de ses propres enfants

**Statut : résolue** — cause confirmée par le journal de production, correctif appliqué et
couvert par des tests.

## Symptôme

Le contrôle `checkOrigin`, ajouté le 2026-08-30, a trouvé sur le stock de production que
**3617 possibilités sur 12 689 (28,5 %)** avaient une racine encore présente : un plateau
dont toutes les cases posées se retrouvent à l'identique dans un plateau plus profond du
même stock. Le sous-arbre du descendant était donc intégralement couvert par sa racine —
le même travail attendait d'être fait deux fois.

Ces 3617 descendants provenaient de **47 racines** seulement, en deux grappes de
profondeur (`alloc` 10–15 et 107–131), et **53 relations** n'avaient qu'**une seule** pièce
d'écart : parent et enfant direct côte à côte dans le stock.

## Faits établis

Tous vérifiés sur `events.log` du serveur de production (2026-07-11 → 2026-08-30) et sur
deux sauvegardes du stock prises à 15:00 et 19:53 le 2026-08-30.

**Le journal montre la séquence, à chaque fois identique :**

```
19:38:16  session de contrôle : CTRL_ACK attendu, reçu -1
19:38:16  session de contrôle déconnectée (slot 2)      <- le canal de contrôle meurt EN PREMIER
19:38:17  client déconnecté (fin de session)  x N        <- les forks de travail se ferment ensuite
19:38:18  bail expiré : 2 possibilité(s) rendue(s) au stock (client disparu)
19:38:18  batch analysed : possibilité non retirée (0, absence confirmée)  x 2
```

**Les comptes concordent exactement, deux fois indépendamment :**

| Vérification | Résultat |
|---|---|
| possibilités rendues par bail sur tout le journal | 50 |
| « absence confirmée » sur tout le journal | 50 |
| expirations précédées d'une fermeture de session de contrôle | 6 / 6 |
| rendues avant la sauvegarde de 15:00 (10+8+10+10) | **38** — ce stock a exactement **38** racines |
| rendues entre les deux sauvegardes (2+10) | **12** — exactement **12** nouvelles racines apparaissent |
| bilan | 38 + 12 − 5 consommées = **45** plateaux racines distincts, le compte du stock de 19:53 |

Les 12 nouvelles racines ont toutes un `alloc` entre 107 et 131 : un lot cohérent, celui
d'un seul client travaillant sur des plateaux avancés. Ce n'est pas une corrélation, c'est
une **identité** — chaque possibilité rendue par un bail expiré est devenue une racine.

**Le constat lui-même a été validé indépendamment** avant d'en chercher la cause : un
oracle écrit en Python relisant directement le `.back` (algorithme distinct de
`is_origin_of` : inclusion de masques de cases posées, puis comparaison des valeurs)
trouve exactement les mêmes 3617 descendants ; après purge, l'ensemble survivant est
identique à celui qu'il prédit et une seconde passe rapporte 0. Les 47 racines et les 3617
descendants sont tous des plateaux **valides** (couleurs des bords concordantes, bordures à
0, aucune pièce en double, `b_faceused` cohérent avec la grille) : ce ne sont ni des
paquets corrompus ni un artefact de décodage.

## Cause

Le serveur jugeait un client vivant sur un seul signal : `owner_control_session_alive`,
qui n'interrogeait que `control_registry_has_active_client` — **la session de contrôle**.

Or, à l'arrêt d'un client, le canal de contrôle (ouvert par le seul process parent) se
ferme **avant** que ses forks de travail aient fini de vider leur file. D'où l'enchaînement :

1. Le client a reçu la possibilité P, l'a explorée, et a **déjà poussé ses enfants** au
   serveur par `INST_ADD`.
2. Son canal de contrôle se ferme. `owner_alive` bascule à faux.
3. Le bail de P était échu depuis longtemps (300 s par défaut ; un plateau profond met
   davantage). Seul `owner_alive` le retenait. La passe suivante réclame P et la remet au
   stock.
4. Le fork, **encore vivant**, envoie enfin son `INST_POSSIBILITY_ANALYSED` pour P. Trop
   tard : P n'est plus dans le pool analysé, d'où le « absence confirmée » — traité comme
   bénin, et qui l'est en soi.
5. P **et** ses enfants sont désormais tous deux en stock. P est la racine de ses propres
   enfants.

Le garde-fou « un client vivant n'expire jamais » (PR7 de la série d'identification des
clients) fonctionnait comme prévu. C'est le **signal de vivacité** qui était faux : il
regardait le canal de contrôle alors que ce sont les forks de travail qui détiennent la
possibilité.

## Correctif

**1. Élargir le signal de vivacité du bail** (`owner_client_alive`, `src/app/etii_server.c`) :
un client est vivant s'il a une session de contrôle enregistrée **ou** au moins une
connexion de travail ouverte (`client_has_open_work_connection`, qui exige
`socket_id != -1` **et** `has_identity` — `has_identity` n'étant remis à zéro qu'à la
réutilisation du slot, s'y fier seul ferait vivre un client indéfiniment et le bail ne
serait plus jamais réclamé, soit le défaut exactement inverse).

`requeue_last_sent_possibility` **garde délibérément l'ancien critère**
(`owner_control_session_alive`, session de contrôle seule) : elle est appelée par la
connexion de travail qui se termine, laquelle serait comptée comme « encore ouverte »
selon l'instant où `socket_id` repasse à -1. Élargir ce critère-là changerait un
comportement qui n'est pas en cause ici.

**2. Nettoyer à la réinjection** (`datamanager_purge_descendants_of`, `src/core/datamanager.c`) :
rendre une possibilité au stock supprime dans la foulée les descendants qu'elle rend
redondants — dans les deux pools de stock **et** dans le pool analysé, un autre client
pouvant travailler sur un descendant devenu inutile. L'origine n'est jamais touchée :
c'est l'arbitrage de `check_origin` (garder la racine, supprimer le descendant — on ne sait
pas ce qui a produit la paire, donc le travail repart de la racine).

Ce second correctif est une **défense en profondeur**, pas un doublon du premier : un
client réellement mort verra toujours son bail réclamé, et c'est là que le nettoyage sert.
Son verrouillage se fait en deux temps (pool analysé, puis stock) sans jamais tenir les
deux familles de verrous ensemble : aucun ordre d'acquisition nouveau, donc aucun risque
d'interblocage avec `INST_GET`. Le prix est une atomicité imparfaite — une possibilité
servie (stock → analysé) entre les deux temps échappe à la passe. C'est un nettoyage au
mieux ; le balayage exhaustif reste `checkOrigin`.

**Coût du verrou, mesuré.** La purge balaie tout le stock sous verrou global, ce que la
série « gestion de charge » interdit pour les opérations longues (un verrou non borné tenu
pendant une sauvegarde multi-Go avait affamé tous les clients au-delà de leur timeout TCP).
Mesure sur le stock de production réel (12 689 possibilités, 10 origines) : **1,0 ms**, soit
**≈ 1,1 s extrapolé à 14 millions** de possibilités, en un seul fil. À comparer au
`--tcp-timeout` de 10 s, et à la fréquence réelle des réclamations : **6 en sept semaines**
de journal. Jugé acceptable sans budget de temps dédié — mais c'est une extrapolation
linéaire depuis un stock 1000 fois plus petit, à revérifier si le stock grossit d'un ordre
de grandeur ou si les réclamations deviennent fréquentes.

## Trouvaille secondaire, corrigée depuis

`datamanager_reclaim_expired_leases` réinjectait par `put(&file_possibility[dest]->file, …)`
— le pool **non vérifié** — sans tenir compte de `checked`. Le paquet se retrouvait donc
dans le pool non vérifié en portant `checked = 1`, était re-vérifié par un pruner pour
rien, et la contradiction entre pool et drapeau ne se résorbait qu'au prochain `restore`,
qui réaiguille, lui, selon le drapeau. C'est ce qui explique que le stock de production
soit passé de 693 possibilités non vérifiées à 0.

`restock_analysed` (commande console `restockAnalysed`) partageait exactement le même
défaut — sa documentation l'annonçait même explicitement. Le troisième chemin de
réinjection, `requeue_last_sent_possibility`, routait déjà correctement puisqu'il passe par
`add_possibility`/`put_to_local` : c'est lui qui a levé le doute sur le comportement voulu.

Corrigé par `put_back_to_stock` (`src/core/datamanager.c`), chemin commun aux deux
réinjections en bloc, qui choisit le pool d'après `checked`. La réinjection ne modifie pas
le plateau : la vérification du pruner porte sur cet état exact et vaut donc toujours —
`checked` n'est remis à 0 que sur un paquet issu d'une **expansion** (`core/possibility.h`).
Trois tests couvrent le routage (paquet vérifié, paquet non vérifié, mélange via
`restockAnalysed`).

Délibérément inchangé au passage : ces deux chemins ne consultent pas le plafond RAM (une
réinjection ne doit jamais pouvoir échouer, sous peine de perdre une possibilité) et
n'alimentent pas les compteurs de débit ADD. Deux écarts par rapport à `put_to_pool`, hors
sujet ici.

## Ce qui reste ouvert

- La **grappe superficielle** (`alloc` 10–15, 17 racines sur 47) est expliquée par le même
  mécanisme et les mêmes comptes, mais aucune capture ne montre le client concerné en
  train de pousser ses enfants juste avant sa disparition : c'est une déduction depuis les
  comptes, pas une observation directe. Rien ne la contredit.
- Le correctif n'a pas été observé en production sur une vraie disparition de client
  depuis son application — seulement couvert par des tests unitaires.
- `checkOrigin` ne balaie que le stock **résident** ; le débordement disque reste hors
  périmètre, cf. [conception/checkorigin_stock_deborde.md](../conception/checkorigin_stock_deborde.md).

## Correction du stock existant

Les 3617 descendants déjà présents ne disparaissent pas d'eux-mêmes : le correctif empêche
d'en créer de nouveaux, il ne nettoie pas l'historique. La manœuvre est `checkOrigin` pour
constater, `checkOrigin purge` pour supprimer les descendants, puis `backup` pour graver
l'état (la purge ne déclenche pas de sauvegarde automatique).
