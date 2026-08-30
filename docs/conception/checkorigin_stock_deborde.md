# `checkOrigin` et le stock débordé sur disque

**Statut : proposition** — non implémenté. Décrit une cible, pas le comportement actuel.

## Le manque

`checkOrigin [purge]` ([console.md](../console.md), `check_origin` dans
`src/core/datamanager.c`) ne balaie que le stock **résident en RAM** : il aplatit les deux
pools (`file_possibility` / `file_possibility_checked`) sous `lock_all_file()` et compare
les paires. Les segments évincés sur disque par `--stock-spill-dir`
([stock_spill.c](../../src/core/stock_spill.c), voir
[utilisation.md](../utilisation.md#plafond-ram-du-stock---stock-max-ram)) ne sont ni
chargés ni comparés.

Conséquence : sur une instance où le débordement est actif, le contrôle rend `0` sans que
cela veuille dire « aucune possibilité n'est la racine d'une autre » — seulement « aucune
parmi celles qui étaient en RAM ». Une racine restée en RAM et son descendant évincé sur
disque (ou l'inverse) passent inaperçus, et une purge laisse le doublon en place.

Ce n'est pas un bug silencieux : `checkOrigin` journalise un avertissement explicite quand
`stock_spill_total_packets() > 0`. Mais un avertissement n'est pas un contrôle.

## Pourquoi c'est différé

Le débordement sur disque **n'est pas exploité en production** à ce jour (arbitrage de
Xavier, 2026-08-30) : le stock de production tient en RAM. Le manque est donc réel mais
sans portée opérationnelle immédiate, alors que le contrôle lui-même a une valeur mesurée
tout de suite (voir ci-dessous).

Traiter le cas maintenant reviendrait à concevoir contre un usage hypothétique, avec le
risque classique de se tromper de contrainte — d'autant que le coût du balayage est en
O(n²) et que la question « comment comparer n paquets dont une partie n'est pas en
mémoire » est indissociable de « quelle taille de stock veut-on réellement contrôler ».

## Ce que le contrôle vaut aujourd'hui (mesuré)

Sur le stock de production du 2026-08-30 (12 689 possibilités, toutes `checked = 1`, aucun
débordement) : **3617 possibilités, soit 28,5 % du stock, ont une racine encore présente**
— 47 racines distinctes, 4118 relations, dont 53 où le descendant n'ajoute qu'**une seule**
pièce (parent et enfant côte à côte dans le stock). Détection en 0,30 s.

Le constat a été validé par un oracle indépendant (relecture directe du `.back`, algorithme
distinct de `is_origin_of` : inclusion de masques de cases posées puis comparaison des
valeurs) qui trouve exactement les mêmes 3617 descendants ; après purge, l'ensemble
survivant est identique à celui prédit par l'oracle, et une seconde passe rapporte 0. Les
47 racines et les 3617 descendants sont tous des plateaux valides (couleurs des bords
concordantes, bordures à 0, aucune pièce en double, `b_faceused` cohérent avec la grille).

## Arbitrage déjà tranché : on supprime les descendants, on garde les racines

`purge` retire le **descendant** et conserve la **racine**, jamais l'inverse. Deux raisons :

1. Le sous-arbre du descendant est inclus dans celui de la racine — le supprimer ne retire
   aucune branche de l'espace de recherche, alors que supprimer la racine en perdrait tout
   le reste.
2. On ne sait pas **pourquoi** ces paires existent. Sans explication du mécanisme qui les a
   produites, on ne peut pas faire confiance à l'état intermédiaire : le travail est à
   refaire depuis la racine.

Le coût assumé est réel : les descendants supprimés sont `checked = 1`, donc on jette du
prunage déjà payé. C'est un choix délibéré, pas un effet de bord — la correction de données
prime sur l'économie de calcul tant que l'origine des doublons n'est pas comprise.

## Pistes, si le débordement devient exploité

Aucune n'est tranchée. Elles sont listées pour ne pas repartir de zéro.

| Piste | Idée | Réserve principale |
|---|---|---|
| Rapatriement complet | Recharger tous les segments avant le balayage | Contredit la raison d'être du plafond RAM : on rétablit exactement ce qu'on avait évincé |
| Balayage par blocs | Charger les segments deux à deux, comparer, relâcher | O(n²) en **E/S** et non plus seulement en CPU ; ordonnancement à concevoir |
| Filtre en deux temps | Une signature compacte par paquet (bien plus petite que 576 o) tenue en RAM pour tout le stock, y compris évincé ; ne recharger que les candidats | Il faut une signature qui soit une condition **nécessaire** de l'inclusion et reste sélective sur des plateaux profonds — un filtre de type Bloom sature à ~130 cases posées sur 256 |
| Contrôle à l'insertion | Refuser à l'entrée du stock une possibilité dont une racine est déjà présente | Change la nature du sujet : ce n'est plus un contrôle a posteriori mais un invariant à tenir, avec son coût dans le chemin chaud |

## Point ouvert, indépendant du débordement

L'**origine** des relations racine/descendant n'est pas établie. Les deux grappes d'`alloc`
observées (10–15 et 107–120) suggèrent deux événements distincts plutôt qu'un phénomène
continu. Hypothèses non vérifiées : reprise d'un bail expiré rendant au stock une
possibilité dont le client avait déjà poussé les enfants, `restockAnalysed`, ou une passe
d'`expand` interrompue. Tant que ce point reste ouvert, la purge traite un symptôme.
