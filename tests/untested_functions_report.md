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

<h2 class="sr-only" style="position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0,0,0,0)">Rapport des fonctions sans tests unitaires — EternityII (49 % de couverture globale)</h2>

<div class="summary-grid">
  <div class="metric"><span class="metric-label">Fonctions non testées</span><span class="metric-value total-fn">98</span><span class="metric-sub">sur 264 au total</span></div>
  <div class="metric"><span class="metric-label">Couverture fonctions</span><span class="metric-value">63 %</span><span class="metric-sub">166 / 264</span></div>
  <div class="metric"><span class="metric-label">Couverture lignes</span><span class="metric-value">49 %</span><span class="metric-sub">2537 / 5179</span></div>
  <div class="metric"><span class="metric-label">Domaine le plus touché</span><span class="metric-value" style="font-size:15px">src/app/</span><span class="metric-sub">0,8 % des lignes</span></div>
</div>

<div class="legend">
  <span class="leg-item"><span class="leg-dot" style="background:#E24B4A"></span>Non testable en unitaire</span>
  <span class="leg-item"><span class="leg-dot" style="background:#EF9F27"></span>Nécessite un PTY / fork réel</span>
  <span class="leg-item"><span class="leg-dot" style="background:#378ADD"></span>Dépend de l'état global runtime</span>
  <span class="leg-item"><span class="leg-dot" style="background:#888780"></span>Appelée uniquement par code non testé</span>
  <span class="leg-item"><span class="leg-dot" style="background:#1D9E75"></span>Puzzle 256 pièces hardcodé</span>
  <span class="leg-item"><span class="leg-dot" style="background:#7F77DD"></span>Branche rarement exécutable</span>
</div>

<!-- RAISON 1 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-red">37 fonctions</span>
    <span class="section-title">Orchestration multi-thread / multi-processus</span>
  </div>
  <div class="reason-box r-red">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions lancent des threads bloquants (<code>accept()</code>, boucles d'événements TCP, IPC fork), gèrent des signaux POSIX, ou sont le <code>main()</code> du programme. Elles ne peuvent pas être appelées en test unitaire sans instancier un processus complet avec durée de vie bornée — les remplacer par des mocks reviendrait à ne pas les tester.</p>
    <div class="file-ref">src/app/etii_client.c — 7 fonctions</div>
    <div class="fn-list" style="margin:4px 0 8px">
      <span class="fn">feed_thread_aposs</span><span class="fn">build_feed_thread</span><span class="fn">control_thread</span><span class="fn">build_control_thread</span><span class="fn">runThreadClient</span><span class="fn">run_mono_client</span><span class="fn">check_client_threads</span>
    </div>
    <div class="file-ref">src/app/etii_server.c — 7 fonctions</div>
    <div class="fn-list" style="margin:4px 0 8px">
      <span class="fn">runserver</span><span class="fn">communicate_with_client</span><span class="fn">create_server_thread</span><span class="fn">rmnonext_thread</span><span class="fn">create_rmnonext_thread</span><span class="fn">get_active_threads</span><span class="fn">check_server</span>
    </div>
    <div class="file-ref">src/app/main.c — 23 fonctions</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">main</span><span class="fn">handle_tcpclient</span><span class="fn">handle_tcpserver</span><span class="fn">handle_test</span><span class="fn">run_client</span><span class="fn">run_auto</span><span class="fn">run_checker</span><span class="fn">fork_checker</span><span class="fn">run_fork_checker</span><span class="fn">init_counters</span><span class="fn">signal_ignored</span><span class="fn">signal_end_handler</span><span class="fn">sigchld_handler</span><span class="fn">init_sigchld_sigaction</span><span class="fn">wait_child</span><span class="fn">server_tcp</span><span class="fn">run_server_thread</span><span class="fn">fork_udp</span><span class="fn">run_fork_thread</span><span class="fn">init_childs</span><span class="fn">init_signals</span><span class="fn">configure_child_signals</span><span class="fn">failed_arg</span>
    </div>
  </div>
</div>

<!-- RAISON 2 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-red">23 fonctions</span>
    <span class="section-title">Interpréteurs de commandes console (dispatch via stdin)</span>
  </div>
  <div class="reason-box r-blue">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions ne sont appelées que via <code>do_command_line()</code> dans la boucle de console interactive. Leur exécution nécessite un état global complet (datamanager initialisé, threads en cours, server_ip configuré). Il serait possible de les tester isolément mais cela demanderait de restituer un contexte global exhaustif — travail faisable mais non encore entrepris.</p>
    <div class="file-ref">src/ui/command_lines.c — 23 interpréteurs</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">sort_ascending_interpreter</span><span class="fn">sort_descending_interpreter</span><span class="fn">limit_interpreter</span><span class="fn">check_interpreter</span><span class="fn">backup_interpreter</span><span class="fn">restore_interpreter</span><span class="fn">import_interpreter</span><span class="fn">loadjson_interpreter</span><span class="fn">print_interpreter</span><span class="fn">sortdm_interpreter</span><span class="fn">split_interpreter</span><span class="fn">regroup_interpreter</span><span class="fn">checkdatas_interpreter</span><span class="fn">check_duplicate_interpreter</span><span class="fn">statistic_interpreter</span><span class="fn">checkfiles_interpreter</span><span class="fn">printfile_interpreter</span><span class="fn">checkfile_interpreter</span><span class="fn">checkdirections_interpreter</span><span class="fn">rmnonext_interpreter</span><span class="fn">printanalysed_interpreter</span><span class="fn">restockanalysed_interpreter</span><span class="fn">min_interpreter</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couverts (via <code>do_command_line</code> dans <code>tests/ui/test_command_lines.c</code>) : <code>sort_ascending</code>, <code>sort_descending</code>, <code>limit</code>, <code>print</code>, <code>sortdm</code>, <code>split</code>, <code>regroup</code>, <code>checkdatas</code>, <code>statistic</code>, <code>checkfiles</code>, <code>printfile</code>, <code>checkfile</code>, <code>checkdirections</code>, <code>printanalysed</code>, <code>restockanalysed</code>, <code>min</code>. Restent : ceux qui exigent un serveur/des fichiers ou appellent <code>exit()</code> (<code>check</code>, <code>backup</code>, <code>restore</code>, <code>import</code>, <code>loadjson</code>, <code>rmnonext</code>, <code>check_duplicate</code>).</p>
  </div>
</div>

<!-- RAISON 3 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-amber">12 fonctions</span>
    <span class="section-title">Moteur de recherche backtracking (état complet requis)</span>
  </div>
  <div class="reason-box r-amber">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions opèrent sur une structure d'état complète : map 4D des pièces, pile de backtracking, contraintes propagées, possibilité en cours. <code>autosearch</code> et <code>search_packet_backtracking</code> sont des boucles d'exploration potentiellement infinies. Les fonctions de délégation TCP (<code>checkAndDelegate…</code>, <code>bt_flush_pending</code>) nécessitent une connexion serveur active.</p>
    <div class="file-ref">src/core/etii_search.c</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">bt_init_constraints</span><span class="fn">bt_propagate_place</span><span class="fn">bt_propagate_undo</span><span class="fn">bt_forward_check</span><span class="fn">bt_count_pending</span><span class="fn">bt_materialize_pending</span><span class="fn">bt_delegate_if_needed</span><span class="fn">bt_flush_pending</span><span class="fn">search_packet_backtracking</span><span class="fn">record_solution</span><span class="fn">checkAndDelegatePossibilitiesIfNeeded</span><span class="fn">checkAndDelegatePossibilitiesIfNeeded_with_big_table</span>
    </div>
    <p style="font-size:12px;color:var(--color-text-secondary);margin:8px 0 0">Note : <code>autoprune</code> et <code>autoprune_gpu</code> sont dans la même situation mais comptabilisés dans src/app/ (0 %). <code>autosearch</code> a 2 lignes couvertes via l'initialisation du thread.</p>
  </div>
</div>

<!-- RAISON 4 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-amber">11 fonctions</span>
    <span class="section-title">Terminal / PTY requis (isatty, ioctl, raw mode)</span>
  </div>
  <div class="reason-box r-amber">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px">Ces fonctions retournent immédiatement ou ne sont jamais appelées si <code>isatty(STDIN_FILENO)</code> ou <code>isatty(STDOUT_FILENO)</code> est faux — ce qui est toujours le cas en CI (stdout est un pipe). Les fonctions de la zone ANSI (<code>redraw_event_zone_locked</code>, <code>event_zone_loop</code>) ne sont atteintes que si <code>zone_active == 1</code>, ce qui nécessite un vrai terminal avec <code>TIOCGWINSZ</code>. Les tester nécessiterait de lancer un pseudo-terminal (PTY) via <code>openpty()</code>.</p>
    <div class="file-ref">src/ui/console.c</div>
    <div class="fn-list" style="margin:4px 0 8px">
      <span class="fn">try_enable_raw_mode</span><span class="fn">getcmdline_raw</span><span class="fn">restore_termios_on_exit</span><span class="fn">console</span><span class="fn">run_console</span>
    </div>
    <div class="file-ref">src/ui/logger.c</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">query_terminal_rows</span><span class="fn">redraw_event_zone_locked</span><span class="fn">event_zone_loop</span><span class="fn">status_zone_init</span><span class="fn">status_zone_teardown</span><span class="fn">clear_console</span>
    </div>
    <p style="font-size:12px;color:var(--color-text-secondary);margin:8px 0 0">Précision : <code>status_zone_init</code>, <code>status_zone_teardown</code> et <code>clear_console</code> ont leur première ligne couverte (garde <code>isatty</code>), mais leur corps réel (manipulation d'écran ANSI) est à 0 %.</p>
  </div>
</div>

<!-- RAISON 5 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-gray">9 fonctions</span>
    <span class="section-title">Commandes datamanager complexes (check_duplicate, sort mthread)</span>
  </div>
  <div class="reason-box r-gray">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>check_duplicate</code> lance N threads pour chercher des doublons sur le stock complet — complexité O(n²) avec des threads ; <code>sort_descending_mthread</code> / <code>sortdmthread</code> sont la variante multi-thread du tri, plus difficiles à vérifier en isolation que leurs équivalents mono-thread (déjà couverts). <code>regroup_datas_nolock</code> / <code>split_datas_nolock</code> ne sont appelées que depuis ces variants non testés.</p>
    <div class="file-ref">src/core/datamanager.c</div>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">check_duplicate</span><span class="fn">check_duplicate_thread</span><span class="fn">run_check_duplicate_thread</span><span class="fn">print_duplicate_args</span><span class="fn">print_duplicate_activity</span><span class="fn">sort_descending_mthread</span><span class="fn">sortdmthread</span><span class="fn">regroup_datas_nolock</span><span class="fn">split_datas_nolock</span>
    </div>
    <p style="font-size:12px;color:#0F6E56;margin:8px 0 0">✓ Désormais couverts (<code>tests/core/test_datamanager.c</code>) : <code>sort_descending_mthread</code>, <code>sortdmthread</code>, <code>split_datas_nolock</code>, <code>regroup_datas_nolock</code> (tri parallèle + variantes « nolock » sous verrou explicite, total préservé), et <code>check_duplicate</code> / <code>check_duplicate_thread</code> / <code>run_check_duplicate_thread</code> (stock vide → retour immédiat ; petit stock distinct → 0). <strong>Bug corrigé</strong> : la boucle de jointure attendait <code>duplicateFinish[t]==1</code> pour les 8 threads alors que seuls les threads réellement lancés réinitialisent ce drapeau ; sur un petit stock (&lt; 8 threads) elle se bloquait indéfiniment. Elle ne joint désormais que les <code>spawned</code> threads effectivement créés.</p>
  </div>
</div>

<!-- RAISON 6 -->
<div class="section">
  <div class="section-header">
    <span class="badge b-gray">4 fonctions</span>
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
    <span class="badge" style="background:#E1F5EE;color:#0F6E56">3 fonctions</span>
    <span class="section-title">Puzzle 256 pièces hardcodé (ETERN_PARTS == 256)</span>
  </div>
  <div class="reason-box r-teal">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>first_possibility</code> génère toutes les positions de départ du puzzle 16×16 à partir de pièces connues (139, 208, 255…). La couverture est mesurée en build 256 pièces (<code>ETERN_PARTS=256</code>) mais cette fonction sort immédiatement si <code>ETERN_PARTS != 256</code>. <code>part_139_i8</code> est un helper qu'elle appelle. <code>prepare_map_part</code> est le point d'entrée de préparation appelé uniquement par <code>etii_client.c</code> et <code>etii_server.c</code>.</p>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">first_possibility</span><span class="fn" style="font-size:11px">src/core/possibility.c</span>
      <span class="fn">part_139_i8</span><span class="fn" style="font-size:11px">src/core/possibility.c</span>
      <span class="fn">prepare_map_part</span><span class="fn" style="font-size:11px">src/core/part.c</span>
    </div>
  </div>
</div>

<!-- RAISON 8 -->
<div class="section">
  <div class="section-header">
    <span class="badge" style="background:#FAEEDA;color:#854F0B">2 fonctions</span>
    <span class="section-title">IPC parent-enfant (log_send_to_parent, import_json)</span>
  </div>
  <div class="reason-box r-purple">
    <div class="reason-label">Raison technique</div>
    <p style="font-size:13px;color:var(--color-text-secondary);margin:0 0 8px"><code>log_send_to_parent</code> n'est appelée que si <code>log_should_route_to_parent()</code> est vrai, ce qui exige simultanément <code>parent_pid != getpid()</code>, <code>fork_checker_socket_id &gt; 0</code> et <code>main_addr != NULL</code> — état d'un processus enfant réel. Possible avec <code>run_in_fork</code> mais complexe à câbler (IPC Unix entre les deux processus). <code>import_json</code> parse un format de solution spécifique depuis <code>STDIN</code>, jamais redirigé en test.</p>
    <div class="fn-list" style="margin:4px 0">
      <span class="fn">log_send_to_parent</span><span class="fn" style="font-size:11px">src/ui/logger.c</span>
      <span class="fn">import_json</span><span class="fn" style="font-size:11px">src/core/datamanager.c</span>
    </div>
  </div>
</div>


Content rendered and shown to the user. Please do not duplicate the shown content in text because it's already visually represented.