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
| [cycle_vie_forks.md](cycle_vie_forks.md) | en cours d'implémentation (3/5 PR livrées) | Cycle de vie dynamique des processus fils côté client : démarrage différé (pas de fork au boot), configuration en console persistée dans un fichier clé=valeur, auto-démarrage 5 s après le boot si ce fichier existe, et reconfiguration à chaud (arrêt des fils, application, re-fork) sans arrêter le process principal. Étude de faisabilité et de risques incluse, découpage en 5 PR. **PR A livrée** ([#183](https://github.com/xaviergriffon/eternityII/pull/183)) : module `client_config` (option `--config-file`, commandes console `config`/`configSave`) — voir `AGENTS.md`/`docs/console.md` pour le comportement actuel de cette seule partie. **PR B livrée** : infrastructure de quiescence coopérative (`src/app/fork_gate.{h,c}`), checkpoints câblés dans les quatre threads candidats et nettoyage des slots morts — voir `AGENTS.md` (*Quiescence infrastructure*). **PR C livrée** : orchestrateur de démarrage différé (`src/app/fork_orchestrator.{h,c}`) — le fork n'a plus lieu au boot, il est décidé par un décompte de 5 s (fichier de config présent) ou par les commandes `start`/`config <clé> <valeur>` ; comprend deux correctifs d'interblocages spécifiques à macOS découverts en testant un vrai client (`fflush(NULL)` contre la console, `flockfile` ne survivant pas à `fork()`) — voir `AGENTS.md` (*Deferred-start orchestrator*). PR D et E (arrêt/redémarrage à chaud, pilotage à distance) restent des propositions. |

### Documents supprimés (entièrement implémentés, contenu absorbé par `docs/`)

| Document | Contenu | Où trouver le comportement actuel |
|---|---|---|
| identification_clients.md | Identification unique des clients au-delà du PID : modèle d'identité (`machine_uid`/`client_uid`/`fork_seq`/`label`/`session_no`), statistiques cumulées persistantes, bail à expiration sur les analyses en cours. 7 PR, toutes livrées. | [echanges_client_serveur.md](../echanges_client_serveur.md) (canal de contrôle, registre de clients connus, attribution et bail), [api_http_rest.md](../api_http_rest.md) (`GET /api/v1/clients`, `/known-clients`), [console.md](../console.md) (`clients`, `clientsWork`, `leaseDuration`, …), [utilisation.md](../utilisation.md) (`--name`, `--machine-uid-file`), et `AGENTS.md` (détail d'implémentation par domaine) |
