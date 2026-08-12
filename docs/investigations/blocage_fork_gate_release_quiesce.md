# Blocage permanent intermittent dans `fork_gate_release_quiesce`

**Statut : ouverte** — cause exacte non confirmée. Deux correctifs structurels appliqués
par prudence (ils réduisent une fenêtre de course plausible), sans preuve qu'ils
éliminent le blocage lui-même. `strace -f` (piste de capture initialement envisagée)
s'est révélé empêcher la reproduction — remplacé par un journal de trace en mémoire,
sans appel système, lisible via `gdb` après coup (voir *Journal de trace en mémoire*
ci-dessous). Voir la convention de ce répertoire dans [README.md](README.md).

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
