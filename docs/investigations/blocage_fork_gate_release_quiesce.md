# Blocage permanent intermittent dans `fork_gate_release_quiesce`

**Statut : résolue.** Cause confirmée grâce au journal de trace en mémoire décrit
ci-dessous (lui-même construit parce que `strace -f` empêchait la reproduction — voir
*`strace -f` empêche la reproduction* plus bas) : `fork_gate_release_quiesce()` était
appelée par le FILS fraîchement forké, pas seulement par le parent — voir *Cause
confirmée* tout en bas pour le détail complet et le correctif. Document conservé (plutôt
qu'absorbé et supprimé, cf. la convention de [README.md](README.md)) : le raisonnement,
les fausses pistes écartées une à une (bug glibc/architecture, famine CPU/thermique,
`SA_RESTART`, boucle de fork sans vérification de `REQUEST_STOP`) et la méthode de
capture gardent de la valeur pour un futur blocage de nature similaire.

## Symptôme signalé par l'opérateur

En exploitation réelle (256 pièces, client avec plusieurs forks, serveur distant, client
lancé en arrière-plan détaché — nohup/screen/tmux/systemd), un ou plusieurs forks de
recherche restent bloqués à 0 sur tous les indicateurs (stock, analysé, coups/s), sans
scénario de reproduction fiable ni trace exploitable dans les logs. Signalé initialement
sans repro ; reproduit plusieurs fois par la suite avec des captures de diagnostic de
plus en plus précises (voir chronologie ci-dessous).

## Diagnostics ajoutés en cours de route (implémentés, documentés dans `docs/`)

Trois filets de sécurité ont été ajoutés pour rendre le symptôme observable — voir
[echanges_client_serveur.md](../echanges_client_serveur.md#diagnostic--forks-vivants-qui-ne-rapportent-rien-après-un-démarrage)
pour le détail complet :

1. **Mort de fork inattendue tracée** (`child_death_record`/`child_death_drain`,
   `src/app/app_runtime.{h,c}`) — ring signal-safe rempli par `sigchld_handler`, drainé et
   loggé par `fork_orchestrator_run`.
2. **Stats IPC orphelines signalées** (`server_tcp`, `src/app/app_runtime.c`) — un
   datagramme dont l'expéditeur ne correspond à aucun fork connu est désormais loggé au
   lieu d'être jeté en silence.
3. **Filet par fork** (`fork_stat_is_zero`/`g_stuck_fork_warned`,
   `src/app/fork_orchestrator.{h,c}`) — alerte un fork spécifique (pid + slot) resté à
   zéro 30s après le (re)fork, remplaçant un premier filet agrégé (« tous à zéro ») qui ne
   se déclenchait jamais sur une panne partielle (le cas réel : certains forks
   travaillent normalement pendant que d'autres sont bloqués).

C'est le filet n°3 qui a permis, à chaque reproduction suivante, d'identifier
immédiatement LE fork en cause (pid + slot) au lieu de devoir deviner.

## Chronologie des reproductions

### Reproduction 1 — faux positif de classification

Log observé : `orchestrateur : fork <pid> disparu de façon inattendue (sortie normale,
code 0)`, alors que ce fork venait de terminer PROPREMENT (exhaustion complète de son
espace de recherche local sur le petit puzzle 16 pièces du test d'intégration — sans
rapport avec `stopForks`/`configApply`). Corrigé (`child_death_is_clean_exit`, résultat
implémenté et documenté normalement dans `docs/`, pas une investigation ouverte).

### Reproduction 2 — corrélation mort de fork ↔ lenteur à répondre à `exit`

Le fork signalé bloqué par le filet n°3 s'est avéré être EXACTEMENT celui qui mettait
>10s à mourir sur `exit` (SIGTERM à 5s, SIGKILL à 10s), pendant que ses frères sains
mouraient en une fraction de seconde. Cause trouvée et corrigée : `configure_child_signals()`
(`src/app/app_runtime.c`, thread `fork_udp` de chaque fork) installait `SIGINT` avec
`SA_RESTART`, contredisant le choix explicite d'`init_signals()` — un signal interrompant
un appel bloquant (le `recvfrom()` sans timeout de `fork_udp`, ou le `connect()` non
borné de `create_tcp_client`) relançait silencieusement l'appel au lieu de renvoyer
`EINTR`, rendant le fork durablement sourd à `REQUEST_STOP`. Corrigé, testé, **résultat
implémenté et documenté normalement** dans
[echanges_client_serveur.md](../echanges_client_serveur.md#correctif--sa_restart-sur-sigint-rendait-certains-forks-sourds-à-larrêt)
— ceci n'est plus une investigation ouverte pour cette partie-là.

### Reproduction 3 — le vrai blocage est dans le process PARENT, pas un fork (ouverte)

Après le correctif `SA_RESTART`, le même symptôme (>10s à mourir sur `exit`) persiste.
Capture en direct sur le pid signalé par le filet n°3 :

```bash
PID=<pid signalé par "ne rapporte aucun travail">
cat /proc/$PID/status | grep -E '^(Threads|State):'
ps -T -p $PID -o tid,stat,wchan:32,comm
sudo gdb -p $PID -batch -ex "thread apply all bt" -ex "detach" -ex "quit"
```

Résultat — **et c'est la surprise** : ce pid n'est pas un fork de recherche mais le
process **PARENT** lui-même (confirmé par `main()` dans la pile : un fork ne repasse
jamais par `main()` après `fork()`, il continue depuis `spawn_child_body`) :

```
Threads:        1
    TID STAT WCHAN                            COMMAND
 <pid>  S+   futex_wait_queue_me              eternityII

Thread 1 (LWP <pid> "eternityII"):
#0  futex_wait (private=<optimized out>, expected=3, futex_word=0xaaaacea5ec60 <g_released+16>) at ../sysdeps/nptl/futex-internal.h:146
#1  futex_wait_simple (...) at ../sysdeps/nptl/futex-internal.h:177
#2  __condvar_quiesce_and_switch_g1 (private=<optimized out>, g1index=<synthetic pointer>, wseq=<optimized out>, cond=0xaaaacea5ec50 <g_released>) at ./nptl/pthread_cond_common.c:276
#3  ___pthread_cond_broadcast (cond=0xaaaacea5ec50 <g_released>) at ./nptl/pthread_cond_broadcast.c:72
#4  0x0000aaaace9260dc in fork_gate_release_quiesce ()
#5  0x0000aaaace926b5c in orchestrator_spawn_forks ()
#6  0x0000aaaace927388 in fork_orchestrator_run ()
#7  0x0000aaaace92eb64 in handle_client ()
#8  0x0000aaaace904928 in main ()
```

## Faits établis (dans l'ordre où ils ont resserré le diagnostic)

1. **Blocage permanent, pas transitoire.** `strace -p <pid>` pendant 30s montre
   EXACTEMENT le même `futex(..., FUTEX_WAIT_PRIVATE, 3, NULL)` en attente, sans jamais
   retourner ; une seconde capture `gdb` 30s plus tard montre la pile identique au
   symbole près. L'opérateur a aussi laissé tourner plusieurs minutes sans que ça ne se
   débloque jamais seul — élimine une simple famine de CPU/ordonnancement transitoire
   (le projet a par ailleurs déjà mesuré du throttling thermique sous charge soutenue,
   cf. `bench-search-node-count` dans `AGENTS.md` — cette piste a été raisonnablement
   envisagée puis écartée par ce test).
2. **Reproduit sur deux architectures et deux versions de glibc très éloignées**
   (aarch64 glibc 2.35 ET x86_64 glibc 2.39 — cette dernière nettement postérieure au
   correctif connu du bug historique de `pthread_cond_broadcast` sur ARM, glibc bug
   25847, corrigé en 2.34). Élimine un bug glibc précis lié à l'architecture ou à une
   version particulière.
3. **Un seul thread vivant dans TOUT le process parent** au moment du blocage
   (`Threads: 1`, confirmé indépendamment par `ps -T` ET par `gdb`'s `thread apply all`)
   — alors qu'un parent en fonctionnement normal en a ~5 (orchestrateur, checker,
   `server_tcp`, canal de contrôle, console). Les quatre autres ont donc disparu AVANT
   ou PENDANT ce blocage précis.
4. **`pthread_cond_broadcast` sur `g_released` ne peut rester bloqué que si un AUTRE
   thread est, à cet instant précis, en train de sortir de son propre
   `pthread_cond_wait` sur ce même `g_released` sans jamais y parvenir** — le seul
   appelant de cette attente est `fork_gate_checkpoint` (`src/app/fork_gate.c`), utilisé
   par les quatre threads de service ci-dessus. Rien dans ce code n'appelle
   `pthread_cancel`/`pthread_kill` sur un thread individuel, et `SIGKILL` sur Linux
   termine tout le groupe de threads d'un coup (jamais un seul thread isolément) — aucun
   mécanisme applicatif connu ne peut donc « tuer » un seul de ces threads en plein
   `pthread_cond_wait` sans terminer le process parent entier (ce qui aurait aussi tué
   le thread orchestrateur, toujours vivant ici).

## Deux pistes structurelles corrigées par prudence — non confirmées comme LA cause

- `console()` (`src/ui/console.c`) se désenregistre et s'arrête DÉFINITIVEMENT dès que
  `getcmdline()` renvoie `NULL` (stdin fermé/EOF) — pertinent puisque ce client tourne en
  arrière-plan détaché (nohup/screen/tmux/systemd, contexte confirmé par l'opérateur).
  Comportement voulu (« le traitement continue sans console »), **non modifié** — cité ici
  seulement parce qu'il illustre qu'un participant `fork_gate` PEUT légitimement
  disparaître en cours de route, un fait qui a orienté la piste suivante.
- **`orchestrator_spawn_forks` ne vérifiait `REQUEST_STOP` à AUCUN moment dans sa boucle
  de création des forks** (`src/app/fork_orchestrator.c`) : un arrêt demandé
  (SIGINT/SIGHUP/SIGTERM — ce dernier atteignant couramment un process lancé en
  arrière-plan détaché) au MILIEU d'un lot de forks n'empêchait pas la boucle de
  continuer à créer les forks restants et à répéter des cycles `quiesce`/`fork`/`release`,
  exactement au moment où d'autres threads réagissent au MÊME signal et se
  désenregistrent de `fork_gate` concurremment — élargissant la fenêtre de course.
  **Corrigé** (PR #194) : la boucle vérifie désormais `request` en tête de chaque
  itération et s'arrête net (sans tenter de fork supplémentaire) dès qu'un arrêt est
  demandé — les forks déjà créés restent valides.

## Ce qui n'est PAS résolu

Ni l'un ni l'autre des deux correctifs n'explique de façon certaine pourquoi
`pthread_cond_broadcast` lui-même reste bloqué pour de bon dans le mécanisme interne de
glibc (`__condvar_quiesce_and_switch_g1`) — seulement pourquoi/quand un participant peut
disparaître, et une piste plausible (mais non prouvée) de fenêtre de course élargie
entre cette disparition et un cycle de quiescence en cours.

## `strace -f` empêche la reproduction — piste abandonnée

La capture initialement envisagée (`strace -f` attaché dès le lancement, journalisant la
création/sortie de chaque thread) a été essayée : **avec `strace`, le blocage ne se
reproduit quasiment jamais ; sans lui, presque à chaque lancement.** L'opérateur a
observé que « la rapidité du lancement semble influencer le problème » — cohérent avec
une vraie course : l'interception ptrace de `strace` ralentit CHAQUE appel système
intercepté (arrêt + relais vers le traceur + reprise), ce qui referme la fenêtre de
course pendant la séquence de démarrage (auto-démarrage puis création rapide des
`nb_forks` forks). `strace` est donc inutilisable pour cette investigation précise —
piste abandonnée.

## Journal de trace en mémoire (`fork_gate_trace_record`) — la piste retenue

Pour observer sans perturber, `src/app/fork_gate.{h,c}` trace désormais chaque
transition d'état dans un ring en mémoire, SANS AUCUN appel système sur le chemin chaud
(`clock_gettime(CLOCK_MONOTONIC)` passe par le VDSO sous Linux — pas de `syscall()` réel
— suivi d'une simple écriture atomique dans un tableau préalloué) : invisible pour
`strace`/ptrace, donc sans effet sur la reproductibilité. La lecture, elle, se fait APRÈS
COUP — une fois le blocage déjà survenu — via `gdb` sur le process vivant (bloqué mais
pas mort) :

```bash
PID=<pid du process PARENT bloqué — celui identifié par le filet par fork, ou repéré
     par "cat /proc/<pid>/status | grep Threads" -> 1 sur un client qui devrait en avoir ~5>

sudo gdb -p $PID -batch \
  -ex "print g_fork_gate_trace_write_index" \
  -ex "print g_fork_gate_trace_buf" \
  -ex "detach" -ex "quit" 2>&1 | tee /tmp/eternityii_fork_gate_trace.txt
```

`g_fork_gate_trace_write_index` donne le nombre total d'événements enregistrés depuis le
démarrage (jamais remis à zéro, y compris après un tour du ring de
`FORK_GATE_TRACE_CAPACITY` = 1024 entrées — seul `(write_index - 1) % 1024` donne
l'entrée la plus récente). Chaque entrée de `g_fork_gate_trace_buf` a :

- `timestamp_ns` — `CLOCK_MONOTONIC`, comparable entre entrées du même process (permet
  de reconstruire l'ordre et les écarts en temps réel entre événements) ;
- `tid` — identifiant de thread noyau (comparable directement au `TID` de `ps -T -p $PID`
  ou du `LWP` affiché par `gdb`) ;
- `slot` — le slot fork_gate concerné (-1 pour les événements globaux comme
  `RELEASE_QUIESCE_BEGIN`/`END`) ;
- `event` — une valeur de `fork_gate_trace_event_t` (`src/app/fork_gate.h`) : `0`=REGISTER,
  `1`=UNREGISTER, `2`=PARK_BEGIN, `3`=PARK_END, `4`=BLOCKED_ON, `5`=BLOCKED_OFF,
  `6`=REQUEST_QUIESCE_BEGIN, `7`=REQUEST_QUIESCE_QUIESCED, `8`=REQUEST_QUIESCE_TIMEOUT,
  `9`=RELEASE_QUIESCE_BEGIN, `10`=RELEASE_QUIESCE_END.

**Preuve directe recherchée dans le journal** : si le blocage est bien dans
`fork_gate_release_quiesce`, la dernière entrée `RELEASE_QUIESCE_BEGIN` (9) du journal
n'a JAMAIS de `RELEASE_QUIESCE_END` (10) correspondant qui la suit — la trace elle-même
le montre, sans avoir besoin de `gdb`'s propre pile d'appels pour le déduire. Croiser
ensuite avec les derniers `PARK_BEGIN`/`PARK_END` (par `tid`, pour voir quel participant
n'a jamais eu de `PARK_END` après son dernier `PARK_BEGIN`) et les derniers
`UNREGISTER` (pour voir si un participant a disparu juste avant) donne la séquence
causale complète, exactement ce que `strace -f` aurait dû montrer sans le pouvoir.

**Piège rencontré à la première utilisation : `gdb` sur le binaire de production
(`make`, sans `-g`) refuse de décoder les types** (`'g_fork_gate_trace_buf' has unknown
type; cast it to its declared type`) — le binaire n'embarque aucune information de
debug, `gdb` ne voit que les symboles bruts. `make DEBUG=1` (mêmes flags d'optimisation
`-Ofast`/`-O3 -ffast-math`, `-g` en plus) résout ça sans changer le timing d'exécution —
contrairement à `strace`, ajouter `-g` ne modifie pas le code généré ni son
ordonnancement. Redéployer ce binaire suffit à faire fonctionner `print
g_fork_gate_trace_buf` normalement. Pour lire les 19 premières entrées précisément (et
uniquement celles-là, sans le bruit des 1005 jamais utilisées) :

```bash
sudo gdb -p $PID -batch \
  -ex "set width 0" -ex "set print elements 0" -ex "set pagination off" \
  -ex "print g_fork_gate_trace_write_index" \
  -ex "print g_fork_gate_trace_buf[0]@<write_index>" \
  -ex "detach" -ex "quit"
```

## Cause confirmée

Première reproduction réelle avec le journal de trace, `write_index = 19` (le blocage
survient très tôt — dans les 19 premiers événements `fork_gate` du démarrage). Décodage
des entrées clés (horodatages en millisecondes relatives à la première entrée) :

```
t≈4006ms  tid=568528  slot=-1  REQUEST_QUIESCE_BEGIN      (2ᵉ cycle de fork — le 1ᵉʳ, entrées 10-15, avait réussi EN ENTIER avec ce même tid)
t≈4204ms  tid=568528  slot=-1  REQUEST_QUIESCE_QUIESCED   (toujours le même tid)
          [fork() a lieu ici]
t≈4205ms  tid=568552  slot=-1  RELEASE_QUIESCE_BEGIN      ← TID DIFFÉRENT. Dernière entrée du journal — jamais de END.
```

Le `tid` de l'appelant de `RELEASE_QUIESCE_BEGIN` (568552) diffère de celui qui venait
de réussir le `REQUEST_QUIESCE` précédent (568528), quelques microsecondes plus tôt,
dans ce qui devrait être un simple appel séquentiel de la même fonction C. Or 568552
correspond EXACTEMENT au `$PID` sur lequel `gdb` était attaché — c'est-à-dire que **le
process observé comme « bloqué » n'était pas le parent, mais le FILS fraîchement forké**,
toujours en train d'exécuter la même fonction (son `main()` hérité du parent est encore
sur sa pile, puisqu'il n'a pas encore atteint la branche `spawn_child_body` qui l'en
détournerait).

En relisant `orchestrator_spawn_forks` (`src/app/fork_orchestrator.c`), la cause saute
aux yeux :

```c
fork_gate_acquire_io_locks();
child_pid = fork();
fork_gate_release_io_locks();
fork_gate_release_quiesce();   // <-- appelée AVANT le test suivant : donc par les DEUX branches

if (child_pid != 0) {
    // ... traitement propre au parent ...
} else {
    // ... spawn_child_body(c), le fils ...
}
```

`fork_gate_release_quiesce()` était placée AVANT le test `if (child_pid != 0)` — donc
exécutée par le FILS aussi bien que par le parent. Le fils hérite (COW) tout l'état de
`fork_gate` (`g_mutex`, `g_released`, `g_slots[]`) au moment du `fork()`. Si l'état
interne de la condvar `g_released` (le compteur/générateur G1/G2 interne à
`pthread_cond_t`, une mécanique privée de glibc) était, À CET INSTANT PRÉCIS, en cours
de transition dans un AUTRE thread du parent — un réveil de `pthread_cond_wait` en train
de se terminer, par exemple — le fils hérite un instantané FIGÉ et INCOHÉRENT de cette
condvar (`fork()` ne clone que le thread appelant ; les autres threads du parent, et
donc leur travail en cours sur cette condvar, n'existent simplement plus dans le fils).
Le fils rappelant alors `pthread_cond_broadcast()` sur cette même condvar peut rester
bloqué à jamais, attendant que la comptabilité interne de glibc soit satisfaite par un
thread qui n'existe plus et ne existera jamais dans ce process.

C'est un piège général et documenté de la combinaison `fork()` + variables de condition
(POSIX ne garantit leur cohérence dans le fils que si elles étaient IDLE — sans aucun
thread en transition dessus — au moment précis du `fork()`), **pas un bug glibc précis**
— ce qui explique la reproduction identique sur deux architectures et deux versions de
glibc très éloignées (glibc 2.35 aarch64 et glibc 2.39 x86_64), et pourquoi `strace -f`
empêchait la reproduction (le ralentissement changeait la probabilité qu'un AUTRE thread
soit exactement en transition sur cette condvar à l'instant du `fork()`).

**Correctif** (`src/app/fork_orchestrator.c`) : `fork_gate_release_quiesce()` n'est plus
appelée que dans la branche parent (`if (child_pid != 0) { fork_gate_release_quiesce();
... }`). Le fils n'a de toute façon rien à en faire : il vient de naître avec un seul
thread, sans le moindre participant `fork_gate` à lui — `fork_gate_release_io_locks()`,
en revanche, reste appelée dans les DEUX branches (elle libère bien SA PROPRE copie
héritée d'un verrou stdio, un besoin réel du fils, sans rapport avec la quiescence).

Les deux correctifs appliqués par prudence pendant l'investigation (`SA_RESTART` retiré,
boucle de fork vérifiant `REQUEST_STOP`) restent en place — ils corrigent des défauts
réels indépendants, découverts en cours de route, même s'ils n'étaient pas LA cause de
ce blocage précis.
