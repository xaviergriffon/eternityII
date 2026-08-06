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
| [identification_clients.md](identification_clients.md) | proposition | Identification unique des clients au-delà du PID : modèle d'identité, statistiques cumulées persistantes, bail à expiration sur les analyses en cours, découpage en 7 PR. |
