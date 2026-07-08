loading_messages: ["Compilation des données gcov…","Croisement avec les signatures…","Mise en forme du rapport…"]
title: rapport_couverture_fonctions_non_testees
widget_code: 
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:var(--font-sans);font-size:14px;color:var(--color-text-primary)}
.section{margin:0 0 2rem}
.section-header{display:flex;align-items:center;gap:10px;margin:0 0 10px;padding-bottom:6px;border-bottom:0.5px solid var(--color-border-tertiary)}
.section-title{font-size:15px;font-weight:500}
.badge{display:inline-flex;align-items:center;font-size:11px;font-weight:500;padding:2px 8px;border-radius:20px;white-space:nowrap}
.b-red{background:#FCEBEB;color:#A32D2D}
.b-amber{background:#FAEEDA;color:#854F0B}
.b-gray{background:#F1EFE8;color:#5F5E5A}
.b-blue{background:#E6F1FB;color:#185FA5}
.reason-box{border-left:2.5px solid;padding:10px 14px;margin:0 0 6px;border-radius:0 6px 6px 0}
.r-red{border-color:#E24B4A;}
.r-amber{border-color:#EF9F27;}
.r-blue{border-color:#378ADD;}
.r-gray{border-color:#888780;}
.r-teal{border-color:#1D9E75;}
.r-purple{border-color:#7F77DD;}
.reason-label{font-size:11px;font-weight:500;text-transform:uppercase;letter-spacing:.04em;margin:0 0 6px;color:var(--color-text-secondary)}
.fn-list{display:flex;flex-wrap:wrap;gap:4px}
.fn{font-family:var(--font-mono);font-size:12px;padding:2px 7px;border-radius:4px;background:rgba(0,0,0,.06);color:var(--color-text-primary)}
.file-ref{font-size:11px;color:var(--color-text-secondary);margin:5px 0 0}
.summary-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin:0 0 2rem}
.metric{background:var(--color-background-secondary);border-radius:8px;padding:12px;display:flex;flex-direction:column;gap:4px}
.metric-label{font-size:12px;color:var(--color-text-secondary)}
.metric-value{font-size:22px;font-weight:500}
.metric-sub{font-size:11px;color:var(--color-text-secondary)}
.total-fn{color:#E24B4A}
.legend{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 1.5rem;font-size:12px;color:var(--color-text-secondary)}
.leg-item{display:flex;align-items:center;gap:5px}
.leg-dot{width:10px;height:10px;border-radius:2px}
</style>

<h2 class="sr-only" style="position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0,0,0,0)">Rapport des fonctions sans tests unitaires — EternityII (73 % de couverture de lignes)</h2>

<div class="summary-grid">
  <div class="metric"><span class="metric-label">Fonctions non testées</span><span class="metric-value total-fn">31</span><span class="metric-sub">sur 283 au total</span></div>
  <div class="metric"><span class="metric-label">Couverture fonctions</span><span class="metric-value">89 %</span><span class="metric-sub">252 / 283</span></div>
  <div class="metric"><span class="metric-label">Couverture lignes</span><span class="metric-value">73 %</span><span class="metric-sub">3836 / 5225</span></div>
  <div class="metric"><span class="metric-label">Couverture branches</span><span class="metric-value">67 %</span><span class="metric-sub">1524 / 2277</span></div>
</div>

<div class="legend">
  <span class="leg-item"><span class="leg-dot" style="background:#E24B4A"></span>Non testable en unitaire</span>
  <span class="leg-item"><span class="leg-dot" style="background:#EF9F27"></span>Nécessite un PTY / fork réel</span>
  <span class="leg-item"><span class="leg-dot" style="background:#378ADD"></span>Dépend de l'état global runtime</span>
  <span class="leg-item"><span class="leg-dot" style="background:#888780"></span>Appelée uniquement par code non testé</span>
  <span class="leg-item"><span class="leg-dot" style="background:#1D9E75"></span>Puzzle 256 pièces hardcodé</span>
  <span class="leg-item"><span class="leg-dot" style="background:#7F77DD"></span>Branche rarement exécutable</span>
</div>

<div class="reason-box r-teal" style="margin:0 0 2rem">
  <div class="reason-label">Constat clé</div>
  <p style="font-size:13px;color:var(--color-text-secondary);margin:0">Les <strong>31 fonctions restantes sont des boucles bloquantes, des points d'entrée, ou des helpers atteignables uniquement via eux</strong> : threads de travail (<code>autosearch</code>, <code>console</code>…), boucles d'événements TCP / <code>accept()</code>, orchestrateurs de <code>fork</code>, l'entry-point <code>main()</code> et ses dispatchers (seule exception, <code>init_client_possibility</code> — un init de struct testable en isolation, mais aujourd'hui appelé seulement par le spawn de threads client non testé). Elles ne sont pas appelables en test unitaire sans instancier un processus complet. <strong>Tout le reste est couvert</strong> — la logique pure a été systématiquement extraite de ces boucles et testée (cf. sections vertes). Domaines : <code>src/core/</code> <strong>99 % des fonctions</strong> (155/157), <code>src/net/</code> <strong>100 %</strong> (13/13) ; le résidu non testé est concentré dans <code>src/app/</code> (boucles client/serveur + <code>main.c</code> à 0 %) et les 2 threads <code>autosearch</code>/<code>autoprune</code> + la console interactive.</p>
</div>

<!-- RAISON 1 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-red">27 fonctions</span>
    <span class="section-title">Orchestration multi-thread / multi-processus</span>
  </div>
  <div class="reason-box r-red">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions lancent des threads bloquants (<code>accept()</code>, boucles d'événements TCP, IPC fork), gèrent des signaux POSIX, ou sont le <code>main()</code> du programme. Elles ne peuvent pas être appelées en test unitaire sans instancier un processus complet avec durée de vie bornée — les remplacer par des mocks reviendrait à ne pas les tester.</p>
    <div class="file-ref">src/app/etii_client.c — 8 fonctions</div>
    <div class="fn-list" style="margin:4px 0 8px">
      <span class="fn">init_client_possibility</span><span class="fn">feed_thread_aposs</span><span class="fn">build_feed_thread</span><span class="fn">control_thread</span><span class="fn">build_control_thread</span><span class="fn">runThreadClient</span><span class="fn">run_mono_client</span><span class="fn">check_client_threads</span>
    </div>
    <div class="file-ref">src/app/etii_server.c — 6 fonctions</div>
    <div class="fn-list" style="margin:4px 0 8px">
      <span class="fn">runserver</span><span class="fn">communicate_with_client</span><span class="fn">create_server_thread</span><span class="fn">rmnonext_thread</span><span class="fn">create_rmnonext_thread</span><span class="fn">check_server</span>
    </div>
    <div class="file-ref">src/app/main.c — 13 fonctions (entry-point : non linké au binaire de test car il définit <code>main()</code>)</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">main</span><span class="fn">handle_tcpclient</span><span class="fn">handle_tcpserver</span><span class="fn">handle_test</span><span class="fn">run_client</span><span class="fn">run_auto</span><span class="fn">run_checker</span><span class="fn">fork_checker</span><span class="fn">run_fork_checker</span><span class="fn">server_tcp</span><span class="fn">run_server_thread</span><span class="fn">fork_udp</span><span class="fn">run_fork_thread</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couvertes (11) : <code>get_active_threads</code> (<code>tests/app/test_etii_server.c</code> — fonction pure déjà liée) ; et, après <strong>extraction de <code>main.c</code> vers le nouveau module <code>src/app/app_runtime.c</code></strong>, les 10 fonctions de plomberie <code>init_counters</code>, <code>init_childs</code>, <code>failed_arg</code>, <code>signal_ignored</code>, <code>signal_end_handler</code>, <code>sigchld_handler</code>, <code>init_sigchld_sigaction</code>, <code>init_signals</code>, <code>configure_child_signals</code>, <code>wait_child</code> (<code>tests/app/test_app_runtime.c</code> — module à <strong>94 %</strong> ; chaque test sauvegarde/restaure la disposition des signaux pour ne pas casser le runner). Restent les <strong>27 fonctions listées</strong> : 26 véritables boucles bloquantes (<code>accept()</code> / <code>while(1)</code> / fork) et l'entry-point <code>main</code> avec ses dispatchers, plus <code>init_client_possibility</code> (init de struct, atteint seulement par le spawn de threads client) — légitimement hors périmètre unitaire.</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ <strong>Découpe des corps de boucle (Tier 1)</strong> : les deux boucles de <em>rapport de stats</em> <code>check_server</code> et <code>check_client_threads</code> restent des coquilles <code>while(1)+sleep</code> intestables, mais leur cœur — la construction des tableaux formatés — a été extrait dans des helpers purs couverts à 100 % : <code>build_file_queues_table</code> (<code>tests/app/test_etii_server.c</code>) et <code>build_thread_queues_table</code> (<code>tests/app/test_etii_client.c</code>). Le débordement de tas historique de la table client est désormais verrouillé par un test <code>NB_THREADS=100</code> sous ASan. La cadence de sauvegarde automatique de <code>check_server</code> (« tous les 6 tours, et seulement si le stock a bougé ») est aussi extraite dans <code>should_autobackup</code> (fonction pure, 100 %, <code>tests/app/test_etii_server.c</code>) : la coquille ne fait plus qu'appeler <code>backup</code>/<code>backup_analysed</code> selon son verdict.</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ <strong>Découpe des corps de boucle (Tier 1, suite — même approche que #51/#52)</strong> : <code>communicate_with_client</code> (boucle d'événements TCP) et <code>control_thread</code> (boucle de régulation) restent des threads bloquants, mais des morceaux purs en ont été extraits et couverts. <code>requeue_last_sent_possibility</code> (<code>tests/app/test_etii_server.c</code>) — le bloc « renvoi au stock des possibilités servies mais non acquittées » de la fin de <code>communicate_with_client</code>, jumeau de <code>requeue_unprocessed_packets</code> (#51) : testé en local (NULL no-op, possibilité en analyse rendue, possibilité acquittée ignorée, lot mixte). <code>control_step</code> (<code>tests/app/test_etii_client.c</code>) — un tour de <code>control_thread</code>, fonction pure sur les globales (<code>counters</code>, <code>max_search_by_sec</code>, <code>request</code>) : testé sur les bascules CONTINUE↔PAUSE selon le débit, le réveil d'un thread inactif et le reset de fenêtre à 1000 tours.</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ <strong>Dispatch protocolaire extrait (Tier 1, suite)</strong> : le corps du <code>while</code> de <code>communicate_with_client</code> (gros <code>if/else if</code> sur l'instruction reçue) est sorti dans <code>communicate_with_client_step</code> (1 = continuer, 0 = arrêter), testé hors thread via un <strong>socketpair</strong> jouant le rôle du socket client (<code>tests/app/test_etii_server.c</code>) : <code>INST_TEST_CONNECTED</code>, handshake de version (ok / refus), instruction inconnue, <code>INST_ADD</code> (ajout au stock), <code>INST_GET</code> (vide → <code>INST_NULL</code> ; servie → retirée du stock), <code>INST_POSSIBILITY_ANALYSED</code>, <code>INST_GET_TO_CHECK</code>, <code>INST_GET_TO_CHECK_BATCH</code> et <code>INST_POSSIBILITY_ANALYSED_BATCH</code> (lot + compte hors borne). <strong>communicate_with_client_step couvert à 51 %</strong> (etii_server.c 13 % → 35 % de lignes en build 256). La branche <code>INST_SOLUTION</code> est désormais couverte dans la suite <strong>16 pièces</strong> (<code>tests/core/test_solution16.c</code>), qui dispose d'une vraie solution 4×4 : la branche est exercée via un <strong>socketpair</strong> + la fixture « golden » — sans <code>--stop-on-solution</code> (sauvegarde CSV + <code>INST_CONSIDERED</code> + retour 1) et avec (<code>run_in_fork</code> + <code>chdir</code> : backup du stock + <code>exit(EXIT_SUCCESS)</code>), fichiers confinés dans un répertoire temporaire. Reste, non couvert en unitaire, le service réel de paquets <em>tocheck</em> (lookup réseau réel).</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ <strong>Corps de boucle d'alimentation extrait (Tier 1, suite)</strong> : le corps de la boucle <code>for</code> de <code>feed_thread_aposs</code> (alimentation des threads de recherche) est sorti dans <code>feed_one_thread</code> (no-op hors REQUEST_CONTINUE ; sinon draine l'« en analyse » puis tente d'obtenir du travail, met à jour <code>works</code>/<code>aposs</code>, sinon keepalive). Testé hors thread en mode local (<code>tests/app/test_etii_client.c</code>) : hors CONTINUE → no-op ; sans travail dispo → demande sans réception ; possibilité dispo → reçue et <code>works=1</code> ; thread occupé sans socket → rien. <strong>feed_one_thread couvert à 66 %</strong> (reste la branche keepalive, qui exige un vrai socket).</p>
  </div>
</div>

<!-- RAISON 2 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">23 fonctions — couvertes</span>
    <span class="section-title">Interpréteurs de commandes console (dispatch via stdin)</span>
  </div>
  <div class="reason-box r-blue">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions ne sont appelées que via <code>do_command_line()</code> dans la boucle de console interactive. Leur exécution nécessite un état global complet (datamanager initialisé, threads en cours, server_ip configuré). Il serait possible de les tester isolément mais cela demanderait de restituer un contexte global exhaustif — travail faisable mais non encore entrepris.</p>
    <div class="file-ref">src/ui/command_lines.c — 23 interpréteurs</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">sort_ascending_interpreter</span><span class="fn">sort_descending_interpreter</span><span class="fn">limit_interpreter</span><span class="fn">check_interpreter</span><span class="fn">backup_interpreter</span><span class="fn">restore_interpreter</span><span class="fn">import_interpreter</span><span class="fn">loadjson_interpreter</span><span class="fn">print_interpreter</span><span class="fn">sortdm_interpreter</span><span class="fn">split_interpreter</span><span class="fn">regroup_interpreter</span><span class="fn">checkdatas_interpreter</span><span class="fn">check_duplicate_interpreter</span><span class="fn">statistic_interpreter</span><span class="fn">checkfiles_interpreter</span><span class="fn">printfile_interpreter</span><span class="fn">checkfile_interpreter</span><span class="fn">checkdirections_interpreter</span><span class="fn">rmnonext_interpreter</span><span class="fn">printanalysed_interpreter</span><span class="fn">restockanalysed_interpreter</span><span class="fn">min_interpreter</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couverts (via <code>do_command_line</code> dans <code>tests/ui/test_command_lines.c</code>) : <code>sort_ascending</code>, <code>sort_descending</code>, <code>limit</code>, <code>print</code>, <code>sortdm</code>, <code>split</code>, <code>regroup</code>, <code>checkdatas</code>, <code>statistic</code>, <code>checkfiles</code>, <code>printfile</code>, <code>checkfile</code>, <code>checkdirections</code>, <code>printanalysed</code>, <code>restockanalysed</code>, <code>min</code>, et désormais <code>check</code>, <code>checkduplicate</code> et <code>loadjson</code> (ce dernier exerce <code>import_json</code> de bout en bout). Et désormais <code>backup</code> / <code>restore</code> / <code>import</code> via un round-trip <code>.back</code> dans un répertoire temporaire (<code>chdir</code> + <code>server = 1</code> pour des noms déterministes, nettoyage avant assertions). Et désormais <code>rmnonext</code> (relit le CSV des pièces <code>parts_files</code> — résolu relativement à la racine du dépôt, CWD de <code>make test</code> ; <code>access()</code> garde-fou qui saute proprement hors racine — reconstruit la map et purge le stock). <strong>23 / 23 couverts.</strong></p>
  </div>
</div>

<!-- RAISON 3 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">12 fonctions — couvertes</span>
    <span class="section-title">Moteur de recherche backtracking (résolu via le build 16 pièces)</span>
  </div>
  <div class="reason-box r-teal">
    <div class="reason-label">Raison technique (obsolète)</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">On pensait ces fonctions intestables : structure d'état complète (map 4D, pile de backtracking, contraintes propagées), <code>search_packet_backtracking</code> vue comme une boucle d'exploration potentiellement infinie, et délégation supposée exiger une connexion serveur active. <strong>Les deux verrous tombent en <code>ETERN_PARTS=16</code></strong> : l'espace 4×4 est fini, donc la recherche lancée depuis la racine vide explore tout l'arbre et <em>retourne</em> (pas de thread, pas de timeout) en passant forcément par <code>record_solution</code> — la solution existe ; et le mode local (<code>server_ip == NULL</code>) fait de <code>send_solution</code> un no-op pendant qu'<code>add_possibility</code> route vers le datamanager local, donc aucun serveur n'est requis pour la délégation.</p>
    <div class="file-ref">src/core/etii_search.c</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">bt_init_constraints</span><span class="fn">bt_propagate_place</span><span class="fn">bt_propagate_undo</span><span class="fn">bt_forward_check</span><span class="fn">bt_count_pending</span><span class="fn">bt_materialize_pending</span><span class="fn">bt_delegate_if_needed</span><span class="fn">bt_flush_pending</span><span class="fn">search_packet_backtracking</span><span class="fn">record_solution</span><span class="fn">checkAndDelegatePossibilitiesIfNeeded</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Couverts (<code>tests/core/test_etii_search.c</code>, etii_search.c passe à 62 % en build 16) : les 5 helpers <code>bt_*</code> + les 2 <code>checkAndDelegate…</code> l'étaient déjà ; ajoutés ici — <code>bt_materialize_pending</code> / <code>bt_delegate_if_needed</code> / <code>bt_flush_pending</code> sur pile montée à la main avec une map uniforme (déterministes, indépendants de la taille) ; <code>search_packet_backtracking</code> + <code>record_solution</code> de bout en bout sur le vrai puzzle 4×4 : la solution est trouvée et enregistrée (fichier <code>solution_*.csv</code>) puis l'arbre est épuisé (retour 0), plus la branche <code>--stop-on-solution</code> (<code>exit(EXIT_SUCCESS)</code> via <code>run_in_fork</code>) et la branche <code>REQUEST_STOP</code> (flush + retour 1).</p>
    <p style="font-size:12px;color:var(--color-text-secondary);margin:8px 0 0">Subtilité : <code>bt_delegate_if_needed</code> est inatteignable par la boucle naturelle en 4×4 (sa fréquence est bornée à 1 000 000 nœuds + <code>DELEGATE_MIN_INTERVAL_MS</code>) — d'où l'appel direct. Restent hors périmètre unitaire dans <code>src/core/etii_search.c</code> (16/18 fonctions, 89 %), ce sont des <strong>boucles de thread</strong> : <code>autosearch</code> (thread de recherche) et <code>autoprune</code> (thread pruner) — ainsi que la branche <code>REQUEST_PAUSE</code> (spin mono-thread). <code>autoprune_gpu</code> est dans <code>gpu_pruner.cu</code> (build CUDA, non couvert).</p>
  </div>
</div>

<!-- RAISON 4 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-amber">2 fonctions</span>
    <span class="section-title">Boucle console interactive (thread + lancement)</span>
  </div>
  <div class="reason-box r-amber">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>console</code> est le <strong>thread de la console</strong> : une boucle <code>while</code> qui lit une ligne (<code>getcmdline</code>) et la dispatche via <code>do_command_line</code> jusqu'à EOF/quit ; <code>run_console</code> ne fait que la lancer en thread détaché. Les exécuter en unitaire reviendrait à piloter une session interactive complète (stdin + état global). En revanche, leurs <strong>briques pures</strong> (lecture de ligne cooked/raw, dispatch des commandes) sont, elles, couvertes isolément.</p>
    <div class="file-ref">src/ui/console.c — boucle de thread + son lanceur</div>
    <div class="fn-list" style="margin:4px 0 8px">
      <span class="fn">console</span><span class="fn">run_console</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Couvert (<code>tests/ui/test_console.c</code>) : le chemin <strong>raw</strong> de la console — <code>try_enable_raw_mode</code>, <code>getcmdline_raw</code> (avec backspace + flèches ↑/↓), <code>restore_termios_on_exit</code> — via un PTY monté avec <code>posix_openpt</code> (POSIX, sans <code>-lutil</code>) ; et <code>getcmdline</code> en mode cooked + EOF. Seules restent non couvertes la boucle <code>console</code> elle-même et son lanceur <code>run_console</code> (ci-dessus).</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ <strong>Tout le cycle de vie de la zone fixe ANSI</strong> est désormais couvert (<code>tests/ui/test_logger.c</code>) : <code>status_zone_init</code>, <code>status_zone_teardown</code>, <code>clear_console</code>, <code>query_terminal_rows</code>, <code>redraw_event_zone_locked</code> et le thread <code>event_zone_loop</code> — exercés dans un fils dont <strong>stdout est l'esclave d'un PTY</strong> (taille fixée via <code>TIOCSWINSZ</code> ; sortie par <code>exit(0)</code> pour que gcov écrive la couverture ; <code>alarm()</code> garde-fou). <code>logger.c</code> passe à <strong>89 % de lignes / 100 % de fonctions</strong>. Restent seulement les branches de redimensionnement et les chemins d'erreur (<code>pthread_create</code>/<code>ioctl</code> en échec).</p>
  </div>
</div>

<!-- RAISON 5 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">9 fonctions — couvertes</span>
    <span class="section-title">Commandes datamanager complexes (check_duplicate, sort mthread)</span>
  </div>
  <div class="reason-box r-gray">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>check_duplicate</code> lance N threads pour chercher des doublons sur le stock complet — complexité O(n²) avec des threads ; <code>sort_descending_mthread</code> / <code>sortdmthread</code> sont la variante multi-thread du tri, plus difficiles à vérifier en isolation que leurs équivalents mono-thread (déjà couverts). <code>regroup_datas_nolock</code> / <code>split_datas_nolock</code> ne sont appelées que depuis ces variants non testés.</p>
    <div class="file-ref">src/core/datamanager.c</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">check_duplicate</span><span class="fn">check_duplicate_thread</span><span class="fn">run_check_duplicate_thread</span><span class="fn">print_duplicate_args</span><span class="fn">print_duplicate_activity</span><span class="fn">sort_descending_mthread</span><span class="fn">sortdmthread</span><span class="fn">regroup_datas_nolock</span><span class="fn">split_datas_nolock</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couverts (<code>tests/core/test_datamanager.c</code>) : <code>sort_descending_mthread</code>, <code>sortdmthread</code>, <code>split_datas_nolock</code>, <code>regroup_datas_nolock</code> (tri parallèle + variantes « nolock » sous verrou explicite, total préservé), et <code>check_duplicate</code> / <code>check_duplicate_thread</code> / <code>run_check_duplicate_thread</code> (stock vide → retour immédiat ; petit stock distinct → 0). Les deux helpers d'affichage sont aussi couverts : <code>print_duplicate_args</code> (transitivement, le petit stock lance un thread via <code>run_check_duplicate_thread</code>) et <code>print_duplicate_activity</code> (appel direct avec compteurs positionnés à la main — en prod elle n'est atteinte qu'après 30 s d'attente d'un thread dans la boucle de jointure). <strong>9 / 9 couverts.</strong> <strong>Bug corrigé</strong> : la boucle de jointure attendait <code>duplicateFinish[t]==1</code> pour les 8 threads alors que seuls les threads réellement lancés réinitialisent ce drapeau ; sur un petit stock (&lt; 8 threads) elle se bloquait indéfiniment. Elle ne joint désormais que les <code>spawned</code> threads effectivement créés.</p>
  </div>
</div>

<!-- RAISON 6 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">4 fonctions — couvertes</span>
    <span class="section-title">Fonctions d'affichage / vérification appelées par les interpreters</span>
  </div>
  <div class="reason-box r-gray">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions ne sont appelées que via les interpréteurs de commandes (eux-mêmes non testés). Elles pourraient être testées directement, mais le verrou global (<code>lock_all_file</code>) et la structure de fichier (<code>File</code>) imposent un état datamanager non vide.</p>
    <div class="file-ref">src/core/datamanager.c</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">check_datas</span><span class="fn">check_one_file</span><span class="fn">check_file</span><span class="fn">check_files</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Couverts (<code>tests/core/test_datamanager.c</code>) : <code>check_datas</code> (stock vide → 0, paquet <code>alloc &gt; ETERN_PARTS</code> → −1), <code>check_file</code> / <code>check_files</code> sur stock vide et peuplé cohérent (→ 0). <code>check_one_file</code> (static) est exercé transitivement ; ses branches d'incohérence interne (size/start désynchronisés) restent non atteintes faute d'API publique pour corrompre une <code>File</code>.</p>
  </div>
</div>

<!-- RAISON 7 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">2 fonctions — couvertes</span>
    <span class="section-title">Puzzle 256 pièces hardcodé (ETERN_PARTS == 256)</span>
  </div>
  <div class="reason-box r-teal">
    <div class="reason-label">Raison technique (résolue)</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>first_possibility</code> génère toutes les positions de départ du puzzle 16×16 à partir de pièces connues (139, 208, 255…). La couverture est mesurée en build 256 pièces (<code>ETERN_PARTS=256</code>) mais cette fonction sort immédiatement si <code>ETERN_PARTS != 256</code>. <code>part_139_i8</code> est un helper qu'elle appelle ; toutes deux exigeaient la vraie map 256 (donc le CSV en CWD) et appelaient <code>exit()</code> directement.</p>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">first_possibility</span><span class="fn" style="font-size:11px">src/core/possibility.c</span>
      <span class="fn">part_139_i8</span><span class="fn" style="font-size:11px">src/core/possibility.c</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ <strong>Les trois verrous tombent</strong> (<code>tests/core/test_possibility.c</code>, build 256). <strong>(1)</strong> Les <code>exit()</code> directs sont passés par un funnel <code>fatal_error()</code> (<code>noreturn</code>, dans le logger), interceptable. <strong>(2)</strong> La dépendance au CSV disparaît grâce à une <strong>map synthétique</strong> <code>make_synthetic_base_256()</code> : 256 pièces algorithmiques dont les 5 indices portent exactement les faces recherchées (139 calibrée pour matcher en rotation 2 → grille 651, l'ancrage exigé par <code>check_possibility</code>), le reste étant des tuiles intérieures identiques. <strong>(3)</strong> L'état global est isolé via <code>run_in_fork</code>. Tests : les <strong>5 pièces-indices manquantes</strong> rendent <code>first_possibility</code> fatale (couvre les 5 sites <code>fatal_error</code> + <code>part_139_i8</code>), et une map valide + un coin posable vérifie l'<strong>injection réelle</strong> (genèse + 1 résultat dans le datamanager, <code>datas_size +1</code>). Restent <code>#####</code>, par construction, les branches défensives « ne devrait jamais arriver » (overflow alloc, paquet incohérent, échec <code>add_possibility</code>).</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Au passage (<code>tests/core/test_part.c</code>) : <code>prepare_map_part</code> — wrapper <code>search_max_face</code> + <code>buildBigArray</code>, indépendant de <code>ETERN_PARTS</code>, testé sur une <code>array_part</code> montée à la main.</p>
  </div>
</div>

<!-- RAISON 8 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">2 fonctions — couvertes</span>
    <span class="section-title">IPC parent-enfant (log_send_to_parent, import_json)</span>
  </div>
  <div class="reason-box r-purple">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>log_send_to_parent</code> n'est appelée que si <code>log_should_route_to_parent()</code> est vrai, ce qui exige simultanément <code>parent_pid != getpid()</code>, <code>fork_checker_socket_id &gt; 0</code> et <code>main_addr != NULL</code>. Un <strong>fork n'est pas strictement nécessaire</strong> : dans un seul process, pointer ces trois globales sur un socket UDP récepteur lié à une adresse temporaire (et <code>parent_pid</code> sur une valeur ≠ <code>getpid()</code>), émettre un log, puis <code>recvfrom</code> le datagramme pour l'asserter.</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couvert (<code>tests/ui/test_logger.c</code>) : <code>log_send_to_parent</code> + <code>log_should_route_to_parent</code> — câblés (sans fork) sur un socket Unix DGRAM récepteur local ; le datagramme (octet de type <code>IPC_MSG_LOG_INFO</code> + texte) est relu par <code>recvfrom</code> et asserté. Exactement l'approche décrite ci-dessus.</p>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couvert (<code>tests/core/test_datamanager.c</code>) : <code>import_json</code>. <strong>Correction du rapport</strong> : il ne lit PAS <code>STDIN</code> mais une chaîne JSON <em>codée en dur</em> dans le module — il draine les files puis ajoute exactement 1 possibilité (vérifié par <code>datas_size()</code>, sûr en build 256 comme 16 car <code>compute_grid</code> borne l'écriture à <code>ETERN_SIZE</code>).</p>
  </div>
</div>


Content rendered and shown to the user. Please do not duplicate the shown content in text because it's already visually represented.