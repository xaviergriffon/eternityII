# Endpoint de découverte des commandes (`scope` × `remote_class`)

**Statut : implémenté** — voir [api_http_rest.md](../api_http_rest.md#get-apiv1commands) pour le comportement de référence à jour. Ce document garde le raisonnement, les approches écartées et les points laissés ouverts.

## Le problème

Une commande console (`pause`, `restore`, `clientsWork`, …) est aujourd'hui classée sur
deux axes indépendants, chacun avec son propre mécanisme :

- **`remote_class`** — comment/si elle voyage sur le réseau : lecture pure sans jeton
  (`control_command_read_only`), modification relayable avec jeton
  (`control_command_allowed`), ou modification strictement serveur/HTTP avec jeton
  (`control_command_privileged`). Porté par `control_command_class_t`
  ([control_protocol.h](../../src/net/control_protocol.h)), documenté comme source unique
  de vérité pour cet axe.
- **`scope`** — où la commande a un sens en local : `client_only` (`start`/`stopForks`/
  `configApply`, pilotage du cycle de vie des fils), `server_only` (champ
  `command_description.server_only`), ou `common`. Ce second axe n'a **pas** de fonction
  unique : il est porté par un champ de struct pour la moitié `server_only`, et par **deux
  fonctions dupliquées** portant le même littéral `{"start","stopForks","configApply"}`
  pour la moitié `client_only` — `command_is_client_only` (côté console) et
  `admin_remote_command_is_client_only` (côté API HTTP admin), toutes deux dans
  [command_lines.c](../../src/ui/command_lines.c), chacune commentée en référence à
  l'autre plutôt que factorisée.

`eternityII_web` (dashboard web tiers, **repo séparé**, hors de ce dépôt git) a besoin des
deux axes pour piloter son UI (interdire un jeton dépensé pour rien, filtrer les commandes
proposables à un client donné) et les **recopie à la main** dans
`internal/eternityclient/commands.go` :
trois listes Go (`StandardCommands`/`PrivilegedCommands`/`ReadOnlyCommands`) miroitant
`remote_class`, plus une quatrième (`ClientPushableCommands`) miroitant `scope` —
préparée mais jamais câblée dans l'UI actuelle (recherche exhaustive dans le repo :
aucune référence hors du fichier qui la déclare et du `CLAUDE.md`). Chaque commande
ajoutée ou retirée côté `eternityII` exige donc une modification synchronisée dans un
second repo, à la main, sans garde-fou si l'un des deux oublie.

## Les deux axes, récapitulatif

| Axe | Question | Valeurs |
|---|---|---|
| `scope` | où la commande a-t-elle un sens en local ? | `common` · `client_only` · `server_only` |
| `remote_class` | comment voyage-t-elle sur le réseau, et est-ce authentifié ? | `read_only` · `write_relayable` · `write_server_only` |

Les deux sont orthogonaux : `restore` est `common` × `write_server_only` (exécutable en
local sur un client, jamais relayable) ; `clientsWork` est `server_only` ×
`read_only` (n'a de sens que sur un serveur, relayable et sans jeton) ; `pause` est
`common` × `write_relayable`.

## Cible : exposer les deux axes comme donnée, pas comme comportement implicite

### Formalisation côté `eternityII` (préalable, indépendant de l'endpoint)

- `control_protocol.c` : les trois tableaux statiques internes à
  `control_command_classify` (`read_only`/`write_relayable`/`write_server_only`) fusionnent
  en une seule table `{nom, classe}`. `control_command_classify` devient une recherche dans
  cette table — comportement inchangé, tests existants inchangés. Nouvelle fonction
  d'énumération :
  ```c
  int control_command_enumerate(const char *out_names[], control_command_class_t out_classes[], int max);
  ```
- `command_lines.h`/`.c` : nouvel axe explicite, même forme que `control_command_class_t` :
  ```c
  typedef enum { CMD_SCOPE_COMMON = 0, CMD_SCOPE_CLIENT_ONLY, CMD_SCOPE_SERVER_ONLY } command_scope_t;
  command_scope_t command_scope_classify(const char *command_name);
  ```
  `command_is_client_only` et `admin_remote_command_is_client_only` disparaissent ; leurs
  ~5 sites d'appel testent directement
  `command_scope_classify(...) == CMD_SCOPE_CLIENT_ONLY`. Contrat : `command_name` est un
  nom seul, sans arguments (pas de tokenisation de ligne complète, à la différence de
  `control_command_classify`) — tous les appelants actuels et prévus n'ont jamais que le
  verbe.
- Le champ `server_only` de `command_description` (~50 entrées positionnelles) n'est **pas**
  touché : ajouter un champ `scope` dessus a déjà été écarté par le passé précisément à
  cause de cette taille (voir le commentaire au-dessus de l'ex-`command_is_client_only`) —
  rien ici ne remet cet arbitrage en cause.

### Nouvel endpoint `GET /api/v1/commands`

Sans authentification (même posture que les autres `GET` : pure métadonnée statique, rien
de sensible). Liste **seulement** les commandes connues de `control_command_classify`
(~27, celles réellement atteignables via le canal de contrôle ou l'API HTTP) — pas les ~50
de la table console entière, dont la majorité (`exit`, `print`, `checkDatas`, …) répond de
toute façon `403`/n'existe pas pour un tiers HTTP.

```json
{
  "commands": [
    {
      "name": "pause",
      "scope": "common",
      "remote_class": "write_relayable",
      "requires_token": true,
      "summary": "pose une pause administrative",
      "usage": null
    },
    {
      "name": "clientsCommand",
      "scope": "server_only",
      "remote_class": "write_relayable",
      "requires_token": true,
      "summary": "relaie une commande à un ou tous les clients connectés",
      "usage": "clientsCommand [--to <cible>] <ligne...>"
    }
  ]
}
```

`requires_token` est un booléen **explicite**, pas dérivé de `remote_class` côté
consommateur : la règle d'authentification a déjà changé une fois indépendamment de cet
axe (historiquement, `write_relayable` et `write_server_only` exigeaient deux niveaux de
jeton différents ; ce n'est plus le cas aujourd'hui, voir
[api_http_rest.md](../api_http_rest.md#post-apiv1command)) — un client qui dériverait
`requires_token` lui-même réintroduirait exactement la duplication qu'on cherche à
éliminer.

Composants :

- `http_server.h`/`.c` : `http_command_info_t` (name/scope/remote_class/requires_token/
  summary/usage) + `int http_commands_collect(http_command_info_t *out, int max)`, fonction
  pure (aucun état vivant à lire — contrairement à `http_clients_collect`, tout est dérivé
  de tables statiques).
- `http_codec.h`/`.c` : `int http_json_format_commands(char *buf, size_t size, const http_command_info_t *infos, int count)`.
- Route `GET /api/v1/commands`, dispatchée comme les autres routes `GET` de
  [http_server.c](../../src/net/http_server.c).

### Tests prévus

- `control_command_enumerate` : 27 entrées, mêmes noms/classes que
  `control_command_classify` appelé individuellement sur chacun (test croisé).
- `command_scope_classify` : les 3 noms `client_only`, un échantillon `server_only`
  (`clientsWork`, `stockMaxRam`) et `common` (`pause`, `restore`).
- `http_commands_collect`/`http_json_format_commands` : sans socket, forme JSON et
  cohérence des champs dérivés sur un échantillon.
- Extension d'un test d'intégration HTTP existant : `GET /api/v1/commands` répond `200`
  sans jeton.

### Docs à mettre à jour une fois implémenté

[api_http_rest.md](../api_http_rest.md) (nouvelle section endpoint + table d'introduction
des fichiers concernés), et une note dans `AGENTS.md` si cet axe `scope` mérite d'y être
documenté au même titre que `control_command_class_t` l'est déjà.

## Arbitrages déjà tranchés

- **Dédupliquer `command_is_client_only`/`admin_remote_command_is_client_only` plutôt que
  d'ajouter une troisième fonction à côté** : la duplication existante est précisément le
  problème qu'on documente ici, en ajouter une troisième instance au moment de construire
  la donnée pour l'endpoint aurait été absurde. Alternative écartée : ajouter un champ
  `scope` à `command_description` — écarté à nouveau pour la même raison que
  `command_is_client_only` l'a été à l'origine (table positionnelle de ~50 entrées).
- **L'endpoint liste seulement les ~27 commandes réseau-pertinentes, pas les ~50 de la
  table console** : les commandes purement locales ne répondent de toute façon jamais
  autre chose qu'un `403` via cette API — les lister n'apporterait aucune information
  exploitable à `eternityII_web`.
- **`requires_token` est un champ explicite, pas dérivé côté client** — voir justification
  ci-dessus (la règle d'auth a déjà divergé de `remote_class` une fois par le passé).
- **`summary`/`usage` sont inclus** (repris tels quels de `command_description`), pour que
  `eternityII_web` puisse construire des tooltips sans dupliquer ce texte non plus — coût
  nul (littéraux déjà en mémoire), ajout purement additif si retiré plus tard n'aurait pas
  eu à changer le contrat.

## Hors de ce document

La consommation côté `eternityII_web` (remplacer les 4 listes Go par un chargement de cet
endpoint au démarrage, stratégie de repli face à un serveur trop ancien pour l'exposer) est
un second sous-projet, dans un second repo, à spécifier séparément une fois cet endpoint
disponible — voir le fil de discussion qui a produit ce document pour le contexte complet
côté `eternityII_web` (`internal/eternityclient/commands.go`).

## Points laissés ouverts

- Dimensionnement exact du buffer JSON de réponse (27 entrées avec `summary`/`usage`) : à
  vérifier contre la borne existante au moment d'écrire le code, pas supposé ici.
- Ordre des entrées dans la réponse : proposé stable, dans l'ordre de la table interne
  (read_only, puis write_relayable, puis write_server_only — même regroupement que les
  tableaux actuels de `api_http_rest.md`), sans tri supplémentaire. À confirmer si
  `eternityII_web` a besoin d'un ordre différent (alphabétique ?) une fois le second
  sous-projet spécifié.
