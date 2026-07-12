cmake_minimum_required(VERSION 3.20)
project(eternityII C)

# Extensions GNU requises (gnu99 = c99 + POSIX + extensions GCC/Clang)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

# ============================================================
# Options — miroir des variables Makefile
# ============================================================
option(DEBUG_BUILD  "Build avec symboles de débogage (-g)"                  OFF)
option(WERROR       "Traite les warnings comme des erreurs (-Werror)"        OFF)
option(NCURSES      "Active l'interface ncurses (remplace logger.c)"         OFF)
option(CUDA         "Active le pruner GPU (requiert le toolkit CUDA)"        OFF)
option(VERIFY       "Vérification croisée GPU/CPU — requiert CUDA=ON"        OFF)
option(ASAN         "AddressSanitizer pour les binaires de test"             OFF)

# Paramètres puzzle/algo (#ifndef-guardés dans static_variables.h → surchargeables)
set(ETERN_PARTS     "" CACHE STRING "Taille du puzzle : 16 ou 256 (défaut 256 du header)")
set(FORWARD_CHECK_K "" CACHE STRING "Profondeur du forward-checking (défaut 6 du header)")

# Architecture CUDA (Jetson Orin Nano = sm_87, comme dans le Makefile)
set(NVCC_ARCH "sm_87" CACHE STRING "Architecture CUDA cible (ex: sm_87)")

# Timeout du test d'intégration (en secondes)
set(INTEGRATION_TIMEOUT "60" CACHE STRING "Timeout (s) pour make test-integration")

# Flags DEBUG_* (décommentés dans static_variables.h en dev local ; CI : -DCPPFLAGS)
option(DEBUG_IN_MONO_PROCESS   "Force single-process (sans fork)"            OFF)
option(DEBUG_SOCKET            "Traces TCP connexion/déconnexion"             OFF)
option(DEBUG_SIGNAL            "Traces gestionnaire de signaux"               OFF)
option(DEBUG_LOCAL_SOCKET      "Traces sockets Unix domaine"                  OFF)
option(DEBUG_THREAD            "Traces création de threads"                   OFF)
option(DEBUG_COMMANDS          "Traces parsing des commandes"                 OFF)
option(DEBUG_CHECK_POSSIBILITY "Valide les paquets de possibilité"           OFF)
option(DEBUG_RM_NO_NEXT        "Traces d'élagage rmnonext"                   OFF)

# ============================================================
# Flags d'optimisation — calqués sur le Makefile
# Sous Darwin, clang déprécie -Ofast → équivalent strict -O3 -ffast-math.
# ============================================================
if(APPLE)
    set(OPT_FLAGS -O3 -ffast-math)
else()
    set(OPT_FLAGS -Ofast)
endif()

# ============================================================
# Flags globaux : warnings uniquement (pas d'optim globale —
# les cibles test/coverage surcharge avec -O2/-O0 via target_compile_options).
# ============================================================
add_compile_options(-Wall -Wextra)
if(WERROR)
    add_compile_options(-Werror)
endif()

# ============================================================
# NCURSES
# ============================================================
if(NCURSES)
    set(LOGGER_SRC src/ui/logger_ncurses.c)
    # Comme le Makefile (`-lncurses`) : lien direct sans find_package pour éviter
    # que CMake résolve vers une libcurses alternative (ex: MacPorts) incompatible.
    set(NCURSES_LIBS ncurses)
else()
    set(LOGGER_SRC src/ui/logger.c)
    set(NCURSES_LIBS "")
endif()

# ============================================================
# CUDA (pruner GPU — Linux/Jetson uniquement)
# ============================================================
if(CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    set(CUDA_APP_SRC src/app/gpu_pruner.cu)
    set(CUDA_LIBS CUDA::cudart stdc++)
    set(CMAKE_CUDA_ARCHITECTURES ${NVCC_ARCH})
    if(WERROR)
        # Pendant exact de `NVCCFLAGS += -Werror all-warnings` dans le Makefile
        string(APPEND CMAKE_CUDA_FLAGS " -Werror all-warnings")
    endif()
else()
    set(CUDA_APP_SRC "")
    set(CUDA_LIBS "")
endif()

# ============================================================
# Sources de production partagées (sans main.c pour réutilisation dans les tests)
# ============================================================
set(PROD_SRCS
    ${LOGGER_SRC}
    src/app/static_variables.c
    src/net/local_socket.c
    src/core/lifo.c
    src/net/tcpclient.c
    src/net/tcpserver.c
    src/core/part.c
    src/core/readdata.c
    src/core/datamanager.c
    src/core/possibility.c
    src/net/etii_protocol.c
    src/net/control_protocol.c
    src/net/http_codec.c
    src/app/etii_client.c
    src/app/etii_server.c
    src/app/etii_control.c
    src/app/control_registry.c
    src/app/app_runtime.c
    src/core/etii_search.c
    src/ui/command_lines.c
    src/ui/command_match.c
    src/ui/command_history.c
    src/ui/console.c
)

# ============================================================
# Exécutable principal
# ============================================================
add_executable(eternityII
    ${PROD_SRCS}
    src/app/main.c
    ${CUDA_APP_SRC}
)
target_include_directories(eternityII PRIVATE src)
target_compile_options(eternityII PRIVATE ${OPT_FLAGS})
target_link_libraries(eternityII pthread ${NCURSES_LIBS} ${CUDA_LIBS})

if(DEBUG_BUILD)
    target_compile_options(eternityII PRIVATE -g)
endif()
if(NCURSES)
    target_compile_definitions(eternityII PRIVATE USE_NCURSES)
endif()
if(CUDA)
    target_compile_definitions(eternityII PRIVATE WITH_CUDA)
    if(VERIFY)
        target_compile_definitions(eternityII PRIVATE GPU_PRUNER_VERIFY)
    endif()
endif()
if(ETERN_PARTS)
    target_compile_definitions(eternityII PRIVATE ETERN_PARTS=${ETERN_PARTS})
endif()
if(FORWARD_CHECK_K)
    target_compile_definitions(eternityII PRIVATE FORWARD_CHECK_K=${FORWARD_CHECK_K})
endif()
foreach(_flag
        DEBUG_IN_MONO_PROCESS DEBUG_SOCKET DEBUG_SIGNAL DEBUG_LOCAL_SOCKET
        DEBUG_THREAD DEBUG_COMMANDS DEBUG_CHECK_POSSIBILITY DEBUG_RM_NO_NEXT)
    if(${_flag})
        target_compile_definitions(eternityII PRIVATE ${_flag})
    endif()
endforeach()

# ============================================================
# Tests unitaires (framework greatest, vendoré dans tests/greatest.h)
# `cmake --build . --target run-tests` → compile + exécute test-16 et test-256.
# `ctest` exécute les tests enregistrés (binaires déjà compilés requis).
# ============================================================
include(CTest)
enable_testing()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Fixture solution16.h : JSON figé → tableau C généré par Python
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solution16.h
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen_solution16.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solution16.json
            > ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solution16.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solution16.json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen_solution16.py
    COMMENT "Génération de tests/fixtures/solution16.h"
)
add_custom_target(solution16_h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solution16.h
)

# Modules de production exercés par les tests (sans main.c, toujours logger.c ANSI)
set(TEST_MODULES
    src/core/lifo.c
    src/core/part.c
    src/core/readdata.c
    src/ui/command_history.c
    src/ui/command_match.c
    src/core/possibility.c
    src/net/etii_protocol.c
    src/net/control_protocol.c
    src/net/http_codec.c
    src/core/datamanager.c
    src/net/local_socket.c
    src/net/tcpclient.c
    src/net/tcpserver.c
    src/ui/command_lines.c
    src/ui/console.c
    src/core/etii_search.c
    src/ui/logger.c
    src/app/static_variables.c
    src/app/app_runtime.c
    src/app/etii_control.c
)

# Suites communes (ETERN_PARTS-agnostiques, jouées en 16 ET 256)
set(TEST_SUITES_COMMON
    tests/core/test_lifo.c
    tests/core/test_part.c
    tests/core/test_readdata.c
    tests/core/test_possibility.c
    tests/core/test_etii_search.c
    tests/core/test_datamanager.c
    tests/net/test_etii_protocol.c
    tests/net/test_control_protocol.c
    tests/net/test_http_codec.c
    tests/net/test_local_socket.c
    tests/net/test_tcp.c
    tests/ui/test_command_history.c
    tests/ui/test_command_match.c
    tests/ui/test_command_lines.c
    tests/ui/test_console.c
    tests/ui/test_logger.c
    tests/app/test_static_variables.c
    tests/app/test_app_runtime.c
    tests/app/test_etii_control.c
)

# Options de compilation pour les tests (-O2 -g, distinct du release -Ofast)
set(TEST_COMPILE_OPTS -Wall -Wextra -O2 -g)
set(TEST_LINK_LIBS pthread m)
if(ASAN)
    list(APPEND TEST_COMPILE_OPTS -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND TEST_LINK_LIBS    -fsanitize=address)
endif()

# Helper : crée un binaire de test avec le ETERN_PARTS voulu.
# Signature : add_unit_test_executable(<target> <etern_parts_or_empty> [extra_srcs...])
function(add_unit_test_executable TARGET ETERN_PARTS_VAL)
    add_executable(${TARGET} EXCLUDE_FROM_ALL
        tests/test_main.c ${TEST_SUITES_COMMON} ${ARGN} ${TEST_MODULES}
    )
    target_include_directories(${TARGET} PRIVATE src tests)
    target_compile_options(${TARGET} PRIVATE ${TEST_COMPILE_OPTS})
    target_link_libraries(${TARGET} ${TEST_LINK_LIBS})
    if(ETERN_PARTS_VAL)
        target_compile_definitions(${TARGET} PRIVATE ETERN_PARTS=${ETERN_PARTS_VAL})
    endif()
endfunction()

# Binaire principal (16) : suites communes + test_solution16, ETERN_PARTS=16
add_unit_test_executable(run_tests_16 16 tests/core/test_solution16.c)
add_dependencies(run_tests_16 solution16_h)

# Binaire secondaire (256) : suites communes, ETERN_PARTS par défaut (256 du header)
add_unit_test_executable(run_tests_256 "")

# Entrées CTest (les binaires doivent être compilés avant `ctest`)
add_test(NAME test-16  COMMAND run_tests_16  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test-256 COMMAND run_tests_256 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Target `run-tests` : compile + exécute (pendant de `make test`)
add_custom_target(run-tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS run_tests_16 run_tests_256
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Tests unitaires : test-16 puis test-256"
)

# ============================================================
# Test d'intégration bout-en-bout (16 pièces, client + serveur TCP)
# `cmake --build . --target test-integration`
# Le script vérifie que les deux côtés voient la solution avec un timeout borné.
# ============================================================

# Binaire ETERN_PARTS=16 dédié — toujours logger ANSI (pas de dépendance ncurses)
add_executable(eternityII16 EXCLUDE_FROM_ALL
    src/ui/logger.c
    src/app/static_variables.c
    src/net/local_socket.c
    src/core/lifo.c
    src/net/tcpclient.c
    src/net/tcpserver.c
    src/core/part.c
    src/core/readdata.c
    src/core/datamanager.c
    src/core/possibility.c
    src/net/etii_protocol.c
    src/net/control_protocol.c
    src/net/http_codec.c
    src/app/etii_client.c
    src/app/etii_server.c
    src/app/etii_control.c
    src/app/control_registry.c
    src/app/app_runtime.c
    src/core/etii_search.c
    src/ui/command_lines.c
    src/ui/command_match.c
    src/ui/command_history.c
    src/ui/console.c
    src/app/main.c
)
target_compile_definitions(eternityII16 PRIVATE ETERN_PARTS=16)
target_include_directories(eternityII16 PRIVATE src)
target_compile_options(eternityII16 PRIVATE ${OPT_FLAGS})
target_link_libraries(eternityII16 pthread)

add_custom_target(test-integration
    COMMAND ${CMAKE_COMMAND} -E env
            "BIN=$<TARGET_FILE:eternityII16>"
            "DATA=${CMAKE_CURRENT_SOURCE_DIR}/data/pieces16.csv"
            "TIMEOUT=${INTEGRATION_TIMEOUT}"
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_solution_16.sh
    COMMAND ${CMAKE_COMMAND} -E env
            "BIN=$<TARGET_FILE:eternityII16>"
            "DATA=${CMAKE_CURRENT_SOURCE_DIR}/data/pieces16.csv"
            "TIMEOUT=${INTEGRATION_TIMEOUT}"
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_control_channel.sh
    DEPENDS eternityII16
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Test d'intégration bout-en-bout (puzzle 16 pièces) : solution + canal de contrôle"
    VERBATIM
)

# ============================================================
# Couverture de code (gcov — intégré gcc/clang, aucune install requise)
# `cmake --build . --target coverage`        → deux passes (256+16) + gcovr --txt fusionné
# `cmake --build . --target coverage-report` → génère XML/HTML/Markdown via gcovr
#
# Les .gcno/.gcda sont produits dans CMAKE_BINARY_DIR/CMakeFiles/<target>.dir/.
# gcovr les retrouve automatiquement via --root / chemin du build.
# ============================================================
set(COV_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/tests/coverage)
set(COV_XML  ${COV_DIR}/coverage.xml)
set(COV_HTML ${COV_DIR}/html)
set(COV_MD   ${COV_DIR}/coverage.md)
set(COV_JSON ${COV_DIR}/coverage-summary.json)
set(COV_COMPILE_OPTS -Wall -Wextra -O0 -g --coverage)

# Helper : crée un binaire de test instrumenté pour la couverture
function(add_coverage_executable TARGET ETERN_PARTS_VAL)
    add_executable(${TARGET} EXCLUDE_FROM_ALL
        tests/test_main.c ${TEST_SUITES_COMMON} ${ARGN} ${TEST_MODULES}
    )
    target_include_directories(${TARGET} PRIVATE src tests)
    target_compile_options(${TARGET} PRIVATE ${COV_COMPILE_OPTS})
    target_link_options(${TARGET} PRIVATE --coverage)
    target_link_libraries(${TARGET} pthread m)
    if(ETERN_PARTS_VAL)
        target_compile_definitions(${TARGET} PRIVATE ETERN_PARTS=${ETERN_PARTS_VAL})
    endif()
endfunction()

# Passe 256 (build par défaut)
add_coverage_executable(run_tests_cov_256 "")

# Passe 16 (suites communes + solution16, ETERN_PARTS=16)
add_coverage_executable(run_tests_cov_16 16 tests/core/test_solution16.c)
add_dependencies(run_tests_cov_16 solution16_h)

# Détection gcovr (utilisé par `coverage` pour le résumé texte et par `coverage-report`)
find_program(GCOVR_EXE gcovr)
# Sur macOS, gcov natif = llvm-cov ; on le signale à gcovr
if(GCOVR_EXE AND APPLE)
    execute_process(
        COMMAND xcrun --find llvm-cov
        OUTPUT_VARIABLE _LLVM_COV OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_LLVM_COV)
        set(GCOVR_GCOV_OPT --gcov-executable "${_LLVM_COV} gcov")
    endif()
endif()

# Target `coverage` : réinitialise les .gcda, exécute les deux passes, affiche
# un résumé gcovr --txt fusionné (union des lignes couvertes par les deux tailles).
# Requiert gcovr : pip install gcovr (ou pipx)
if(GCOVR_EXE)
    add_custom_target(coverage
        COMMAND find ${CMAKE_BINARY_DIR}/CMakeFiles -name "*.gcda" -delete
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
                $<TARGET_FILE:run_tests_cov_256>
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
                $<TARGET_FILE:run_tests_cov_16>
        COMMAND ${GCOVR_EXE}
                ${GCOVR_GCOV_OPT}
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --exclude "(^|/)tests/"
                ${CMAKE_BINARY_DIR}
                --txt
        DEPENDS run_tests_cov_256 run_tests_cov_16
        COMMENT "Couverture : passe 256 + passe 16 → résumé gcovr fusionné"
        VERBATIM
    )
else()
    add_custom_target(coverage
        COMMAND find ${CMAKE_BINARY_DIR}/CMakeFiles -name "*.gcda" -delete
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
                $<TARGET_FILE:run_tests_cov_256>
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
                $<TARGET_FILE:run_tests_cov_16>
        DEPENDS run_tests_cov_256 run_tests_cov_16
        COMMENT "Couverture : passe 256 + passe 16 (gcovr introuvable — pas de résumé texte)"
    )
    message(STATUS "gcovr introuvable : `coverage` ne produira pas de résumé texte (pip install gcovr)")
endif()

# Target `coverage-report` : génère XML (Codecov), HTML et Markdown via gcovr
# Requiert gcovr : pip install gcovr (ou pipx)
if(GCOVR_EXE)
    add_custom_target(coverage-report
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COV_HTML}
        # Fusionne les .gcda des deux passes (BUILD_DIR contient les deux arborescences).
        # VERBATIM : CMake quote proprement les arguments (regex avec parenthèses, etc.)
        COMMAND ${GCOVR_EXE}
                ${GCOVR_GCOV_OPT}
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --exclude "(^|/)tests/"
                ${CMAKE_BINARY_DIR}
                --txt
                --cobertura ${COV_XML}
                --html-details ${COV_HTML}/index.html
                --json-summary ${COV_JSON}
                --markdown ${COV_MD}
                --markdown-title "Couverture de code"
        # Post-traitement : section « Couverture par domaine » dans le Markdown
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/coverage_by_domain.py
                ${COV_JSON} ${COV_MD}
        DEPENDS coverage
        COMMENT "Rapport gcovr : Cobertura XML + HTML + Markdown"
        VERBATIM
    )
else()
    message(STATUS "gcovr introuvable : la cible coverage-report ne sera pas disponible (pip install gcovr)")
endif()

