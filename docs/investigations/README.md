# Investigations en cours

Ce répertoire rassemble les documents d'**investigation** : la trace d'une chasse à un
bogue difficile à reproduire — faits établis, hypothèses écartées, correctifs appliqués
« par prudence » sans certitude qu'ils règlent la cause exacte, et pistes restantes.

Distinction avec [`docs/`](../) : un document de `docs/` répond à « comment ça marche
aujourd'hui ? » et doit être exact à tout instant. Distinction avec
[`docs/conception/`](../conception/) : un document de conception répond à « comment on
voudrait que ça marche, et pourquoi ces choix-là ? » pour une fonctionnalité qui n'existe
pas encore. Un document d'ici répond à « qu'est-ce qu'on sait, et qu'est-ce qu'on ne sait
toujours pas, sur ce bogue précis ? » — le code documenté ici EXISTE et se comporte
normalement dans l'immense majorité des cas ; seul un symptôme précis, intermittent,
reste incomplètement expliqué.

## Convention

- **Statut en tête de document** : `ouverte` (cause exacte non confirmée, correctifs
  appliqués par prudence ou aucun) ou `résolue` (cause confirmée et corrigée — garder le
  document tant que le raisonnement/les fausses pistes ont de la valeur, sinon absorber
  le contenu utile dans `docs/` et supprimer).
- **Faits établis vs. hypothèses** : distinguer explicitement ce qui a été *prouvé* par
  une observation reproductible (capture `gdb`/`strace`, log, test) de ce qui reste une
  hypothèses non vérifiée — ne jamais présenter une hypothèse comme un fait.
- **Correctifs appliqués sans certitude** : un correctif structurel appliqué "par
  prudence" pendant une investigation encore ouverte doit rester documenté ICI, pas dans
  `docs/`, tant que sa relation exacte avec le symptôme n'est pas confirmée — `docs/`
  documente le comportement du code, pas les hypothèses en cours de test.
- Une PR ultérieure qui confirme ou infirme une piste **met à jour ce document**, elle ne
  le laisse pas mentir. Quand la cause est enfin confirmée et corrigée, le document passe
  au statut `résolue` et son contenu utile (la cause réelle, le correctif définitif) migre
  vers `docs/` ou `AGENTS.md` comme documentation de comportement normal.

## Documents

| Document | Statut | Sujet |
|---|---|---|
| [bail_expire_racines_en_stock.md](bail_expire_racines_en_stock.md) | résolue | 28,5 % du stock de production (3617 possibilités sur 12 689) avaient une racine encore présente. Cause : le bail jugeait un client vivant sur sa seule session de contrôle, laquelle se ferme AVANT que ses forks de travail aient fini d'émettre — le serveur rendait au stock une possibilité dont le client avait déjà poussé les enfants, le parent devenant la racine de ses propres enfants. Prouvé par identité de comptes sur le journal (50 rendues = 50 « absence confirmée », 6/6 précédées d'une fermeture de session, 38 puis 12 racines apparues exactement en face des 38 puis 12 rendues). Corrigé en élargissant la vivacité aux connexions de travail ouvertes, plus un nettoyage ciblé des descendants à la réinjection. Conservé pour la méthode (oracle indépendant validant le constat avant d'en chercher la cause) et pour la trouvaille secondaire non corrigée : la réinjection place le paquet dans le pool non vérifié sans remettre `checked` à 0. |
| [blocage_fork_gate_release_quiesce.md](blocage_fork_gate_release_quiesce.md) | résolue | Blocage permanent intermittent dans `fork_gate_release_quiesce`, causé par son appel depuis le FILS fraîchement forké (condvar héritée en état incohérent) — conservé pour le raisonnement, les fausses pistes écartées et la méthode de capture (journal de trace en mémoire, `strace` empêchant la reproduction) |
