# Conception et réflexions en cours

Ce répertoire rassemble les documents de **conception** : propositions, arbitrages,
découpages en PR. Ils décrivent une **cible**, pas le comportement du code.

Distinction avec [`docs/`](../) : un document de `docs/` répond à « comment ça marche
aujourd'hui ? » et doit être exact à tout instant ; un document d'ici répond à « comment
on voudrait que ça marche, et pourquoi ces choix-là ? » et peut parfaitement décrire
quelque chose qui n'existe pas encore.

## Convention

- **Statut en tête de document**, explicite : `proposition`, `en cours d'implémentation`,
  ou `implémenté` (avec le lien vers la doc de référence qui prend le relais).
- Consigner les **arbitrages tranchés** avec leur raison, pour qu'ils n'aient pas à être
  rediscutés à chaque PR, et lister à part les **points laissés ouverts**.
- Une PR d'implémentation qui invalide un arbitrage **met à jour ce document**, elle ne
  le laisse pas mentir.

## Cycle de vie

Quand le contenu est entièrement implémenté, le comportement rejoint la documentation de
référence de `docs/` (ou `AGENTS.md`). Deux options selon l'intérêt résiduel du
document : le supprimer si `docs/` couvre tout, ou le garder marqué `implémenté` quand il
conserve de la valeur — le raisonnement, les alternatives écartées et les mesures ne
survivent nulle part ailleurs.

## Documents

| Document | Statut | Contenu |
|---|---|---|
| [maitrise_charge_serveur.md](maitrise_charge_serveur.md) | en cours d'implémentation (PR 1-4/5 livrées) | Une sauvegarde qui tient le verrou global du stock pendant toute sa durée affamait les clients jusqu'à leur timeout TCP sous forte charge. PR 1 : borne les boucles trylock jusque-là infinies de `datamanager.c`, ajoute `setvbuf` sur le `FILE*` de sauvegarde et l'option `--tcp-timeout`. PR 2 : `consistent_backup`, gel global des deux pools à un instant T unique puis libération progressive file par file (jamais de `fork()`/COW, coût mémoire nul). PR 3 : rééquilibrage incrémental borné en temps entre les files (`--rebalance-budget`, commande console `rebalance [n]`), rejouable plusieurs fois par tour sous un budget total. PR 4 : nombre de files configurable au démarrage (`--stock-files`), stockage en tableaux de pointeurs alloués dynamiquement — un bogue SIGILL réel (`report` de taille fixe débordant avec un grand nombre de files) trouvé et corrigé en vérifiant le binaire réel. PR 5 (proposition, non implémentée) : éviter une sauvegarde inutile à la granularité du fichier. |
| [elagage_recherche.md](elagage_recherche.md) | implémenté (9/9 PR exécutées) | Élargir l'élagage au-delà du forward-check de proximité. PR 1/9 livrée : voisines géométriques plutôt que fenêtre de parcours (+68,8 % de débit, taux d'élagage quasi inchangé). PR 2/9 (conflit de singletons) et PR 3/9 (typage coin/bord, variante compteurs) évaluées et écartées après mesure (−9 % et −14 % de débit, 0 déclenchement). PR 4/9 (comptage global couleur, implémentation complète consciente des ancêtres) écartée aussi (−24 % de débit) malgré un déclenchement massif (≈47-49 % des élagages) — recoupe structurellement le forward-check. PR 5/9 livrée : point fixe dans le balayage du pruner (`possibility_all_has_a_next_counted`) — sur un stock réel, `master` n'atteint le vrai point fixe qu'après 2 appels `rmnonext`, la version à point fixe l'atteint en 1 seul appel (1193→950 vs 1193→965→950), surcoût par appel largement absorbé. PR 6/9 implémentée/mesurée mais désactivée par défaut : DFS à budget dans le pruner, 0 % de fermeture sur stock réel à n'importe quel budget testé (jusqu'à 1 000 000 de nœuds) — code conservé opt-in (`pruner_dfs_budget`), mur structurel `max_result` ≈ 74. PR 7/9 livrée : tri des candidats de chaque compartiment de l'arène par rareté de couleur exposée croissante, à la construction de la map (+3,2 % de débit médian moyen, aucun coût dans la boucle chaude, adopté inconditionnellement). PR 8/9 implémentée sans concession, testée (même ensemble de solutions activée/désactivée sur le vrai puzzle 4×4) et écartée : propagation des cases forcées dans la boucle chaude — −40,4 % de débit et `max_result` légèrement inférieur à budget de nœuds égal, le mécanisme recoupant le forward-check au point de doubler son coût de lookup par placement sans que la réduction de branchement ne compense au mur structurel actuel. PR 9/9 : prototype scopé d'ordre dynamique (MRV) concluant mais non déployé (délégation désactivée) — `max_result` 74→180 à 5 M nœuds (déjà 173 à 500 k), débit −99,7 % (coût du balayage naïf, pas du principe), code conservé derrière `ETII_MRV=1` ; implémentation complète (cache incrémental, re-canonisation aux frontières de délégation) recommandée comme projet séparé. Reste en proposition : typage coin/bord par partition de l'arène. |

Voir aussi *Documents supprimés* ci-dessous pour ceux entièrement implémentés et absorbés
par `docs/`.

### Documents supprimés (entièrement implémentés, contenu absorbé par `docs/`)

| Document | Contenu | Où trouver le comportement actuel |
|---|---|---|
| identification_clients.md | Identification unique des clients au-delà du PID : modèle d'identité (`machine_uid`/`client_uid`/`fork_seq`/`label`/`session_no`), statistiques cumulées persistantes, bail à expiration sur les analyses en cours. | [echanges_client_serveur.md](../echanges_client_serveur.md) (canal de contrôle, registre de clients connus, attribution et bail), [api_http_rest.md](../api_http_rest.md) (`GET /api/v1/clients`, `/known-clients`), [console.md](../console.md) (`clients`, `clientsWork`, `leaseDuration`, …), [utilisation.md](../utilisation.md) (`--name`, `--machine-uid-file`), et `AGENTS.md` (détail d'implémentation par domaine) |
| cycle_vie_forks.md | Cycle de vie dynamique des processus fils côté client : démarrage différé (pas de fork au boot), configuration en console persistée dans un fichier clé=valeur, auto-démarrage 5 s après le boot si ce fichier existe, reconfiguration à chaud (arrêt des fils, application, re-fork) sans arrêter le process principal, et pilotage à distance depuis le serveur (console locale, canal de contrôle, API HTTP admin). | [console.md](../console.md) (`config`, `configSave`, `start`, `stopForks`, `configApply`), `AGENTS.md` (*Quiescence infrastructure*, *Deferred-start orchestrator*, *Remote piloting* — détail d'implémentation), [echanges_client_serveur.md](../echanges_client_serveur.md) (*Pilotage à distance du cycle de vie des fils*), et [api_http_rest.md](../api_http_rest.md) |
