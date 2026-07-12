ifeq ($(OS),Windows_NT) 
    detected_OS := Windows
else
    detected_OS := $(shell sh -c 'uname 2>/dev/null || echo Unknown')
endif

# Les sources vivent sous src/<domaine>/ (core, net, ui, app). Les objets sont
# générés sous build/ en miroir de src/ (build/ est gitignoré). -Isrc rend les
# includes explicites "domaine/x.h" résolvables depuis n'importe quel domaine.
BUILD_DIR := build

OPENCLLIB := -lOpenCL
ifeq ($(detected_OS),Darwin)
	OPENCLLIB := -framework OpenCL
endif

# etii_opencl.o ${OPENCLLIB}

# Flag d'optimisation : -Ofast sous Linux (gcc). Sous Darwin, clang déprécie
# -Ofast (warning -Wdeprecated-ofast) et recommande son équivalent strict.
OPTFLAGS := -Ofast
ifeq ($(detected_OS),Darwin)
	OPTFLAGS := -O3 -ffast-math
endif

# Ajout d'une variable DEBUG pour activer ou désactiver les informations de débogage
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS= -Wall -Wextra -std=gnu99 $(OPTFLAGS) -Isrc -g
	CLEAN_OBJS =
else
    CFLAGS= -Wall -Wextra -std=gnu99 $(OPTFLAGS) -Isrc
	CLEAN_OBJS = rm -rf $(BUILD_DIR)
endif

# Traite les warnings comme des erreurs. Désactivé par défaut : le build local
# reste tolérant (et clang n'émet de toute façon pas les mêmes diagnostics que
# GCC). La CI Linux/GCC appelle `make WERROR=1` pour bloquer toute régression :
# l'enforcement vit là où tournent les analyses GCC (-Wstringop-truncation,
# -Wformat-truncation, -Wuse-after-free, …) que clang ne reproduit pas.
WERROR ?= 0
ifeq ($(WERROR),1)
    CFLAGS += -Werror
endif

EXECUTABLE ?= eternityII

# Active l'interface ncurses optionnelle (par défaut : interface ANSI sans
# dépendance externe). Avec NCURSES=1, on compile logger_ncurses.c à la place
# de logger.c et on lie -lncurses. Le binaire reste 100% compilable sans
# ncurses tant que NCURSES est laissé à 0.
NCURSES ?= 0
ifeq ($(NCURSES),1)
    CFLAGS += -DUSE_NCURSES
    LOGGER_OBJ := logger_ncurses.o
    NCURSES_LIB := -lncurses
else
    LOGGER_OBJ := logger.o
    NCURSES_LIB :=
endif

# Active le pruner GPU optionnel (mode `gpupruner`). Sans CUDA=1 : aucun .cu
# compilé, -DWITH_CUDA absent, binaire strictement identique au build classique.
# Avec CUDA=1 : compile gpu_pruner.cu avec nvcc et lie le runtime CUDA. Calqué
# sur le switch NCURSES ci-dessus. Cible : Linux/Jetson (NVIDIA) — pas Darwin.
# Surcharges utiles : NVCC, CUDA_PATH, NVCC_ARCH (Orin Nano = sm_87).
CUDA ?= 0
ifeq ($(CUDA),1)
    NVCC ?= nvcc
    NVCC_PATH := $(shell command -v $(NVCC) 2>/dev/null)
    ifeq ($(NVCC_PATH),)
        $(error CUDA=1 mais '$(NVCC)' est introuvable dans le PATH. Installez le toolkit CUDA ou indiquez NVCC=/chemin/vers/nvcc)
    endif
    CUDA_PATH ?= /usr/local/cuda
    NVCC_ARCH ?= sm_87
    NVCCFLAGS ?= -O3 -arch=$(NVCC_ARCH)
    # WERROR=1 propage la sévérité au compilateur GPU : `-Werror all-warnings`
    # transforme tout diagnostic nvcc du code device (cross-execution-space-call,
    # déclarations dépréciées, réordonnancements…) en erreur — pendant exact du
    # -Werror gcc côté C. Requiert nvcc >= 11.2 (CI : CUDA 12.5).
    ifeq ($(WERROR),1)
        NVCCFLAGS += -Werror all-warnings
    endif
    CFLAGS += -DWITH_CUDA
    CUDA_OBJ := $(BUILD_DIR)/app/gpu_pruner.o
    CUDA_LIB := -L$(CUDA_PATH)/lib64 -lcudart -lstdc++
else
    CUDA_OBJ :=
    CUDA_LIB :=
endif

# Vérification croisée GPU vs CPU (parité stricte) : pour chaque lot, rejoue le
# contrôle CPU `possibility_all_has_a_next` et logue toute divergence (vivant/mort
# ou contenu muté). N'a de sens qu'avec CUDA=1. Laisser VERIFY=0 en production.
VERIFY ?= 0
ifeq ($(VERIFY),1)
    CFLAGS += -DGPU_PRUNER_VERIFY
endif

# Liste des objets, en miroir de src/<domaine>/. $(LOGGER_OBJ) (logger.o ou
# logger_ncurses.o) vit dans ui/ ; $(CUDA_OBJ) (vide sauf CUDA=1) dans app/.
OBJS := \
	$(BUILD_DIR)/ui/$(LOGGER_OBJ) \
	$(BUILD_DIR)/app/static_variables.o \
	$(BUILD_DIR)/net/local_socket.o \
	$(BUILD_DIR)/core/lifo.o \
	$(BUILD_DIR)/net/tcpclient.o \
	$(BUILD_DIR)/net/tcpserver.o \
	$(BUILD_DIR)/core/part.o \
	$(BUILD_DIR)/core/readdata.o \
	$(BUILD_DIR)/core/datamanager.o \
	$(BUILD_DIR)/core/possibility.o \
	$(BUILD_DIR)/net/etii_protocol.o \
	$(BUILD_DIR)/net/control_protocol.o \
	$(BUILD_DIR)/app/etii_client.o \
	$(BUILD_DIR)/app/etii_server.o \
	$(BUILD_DIR)/app/control_registry.o \
	$(BUILD_DIR)/app/app_runtime.o \
	$(BUILD_DIR)/core/etii_search.o \
	$(BUILD_DIR)/ui/command_lines.o \
	$(BUILD_DIR)/ui/command_match.o \
	$(BUILD_DIR)/ui/command_history.o \
	$(BUILD_DIR)/ui/console.o \
	$(BUILD_DIR)/app/main.o \
	$(CUDA_OBJ)

$(EXECUTABLE): $(OBJS)
	gcc -pthread -o $(EXECUTABLE) $(OBJS) ${CFLAGS} ${CPPFLAGS} $(NCURSES_LIB) $(CUDA_LIB)
	$(CLEAN_OBJS)

# Règle motif : build/<domaine>/x.o à partir de src/<domaine>/x.c. -Isrc est
# déjà dans CFLAGS. mkdir -p crée build/<domaine>/ à la volée.
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation du module GPU (uniquement requis lorsque CUDA=1 ; cette règle n'est
# jamais invoquée sans CUDA=1 car $(CUDA_OBJ) est alors vide).
$(BUILD_DIR)/app/gpu_pruner.o: src/app/gpu_pruner.cu src/app/gpu_pruner.h src/core/possibility.h src/core/part.h src/app/static_variables.h src/ui/logger.h
	@mkdir -p $(dir $@)
	$(NVCC) $(NVCCFLAGS) -DWITH_CUDA -Isrc -c src/app/gpu_pruner.cu -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(EXECUTABLE)
	rm -f eternityII16
	rm -f $(TEST_BIN) $(TEST_BIN_16)
	rm -f $(SOLUTION16_H)
	rm -rf $(COV_DIR) $(COV_DIR_16)
	rm -f *.gcov *.gcno *.gcda

# ---------------------------------------------------------------------------
# Tests unitaires (framework greatest, vendoré dans tests/greatest.h).
#
# `make test` compile un binaire isolé qui ne tire QUE les modules sous test
# et leurs dépendances de link (pas de main.c). Aucune dépendance externe à
# installer : greatest est un header unique. -lm car part.c référence pow()
# (non inliné sans optimisation) ; -pthread pour les mutex de logger.c.
# ---------------------------------------------------------------------------
TEST_BIN     := tests/run_tests
TEST_BIN_16  := tests/run_tests_16
# test_main.c (runner) + greatest.h/fork_assert.h restent à la racine de tests/ ;
# les suites sont rangées par domaine, en miroir de src/.
# TEST_RUNNER : le point d'entrée greatest (commun aux deux binaires).
# TEST_SUITES_COMMON : les suites indépendantes de ETERN_PARTS (jouées en 16 ET 256).
# TEST_SOLUTION16 : la suite « solution réelle », valide UNIQUEMENT en build 16.
TEST_RUNNER  := tests/test_main.c
TEST_SUITES_COMMON := \
                tests/core/test_lifo.c tests/core/test_part.c tests/core/test_readdata.c tests/core/test_possibility.c tests/core/test_etii_search.c tests/core/test_datamanager.c \
                tests/net/test_etii_protocol.c tests/net/test_control_protocol.c tests/net/test_local_socket.c tests/net/test_tcp.c \
                tests/ui/test_command_history.c tests/ui/test_command_match.c tests/ui/test_command_lines.c tests/ui/test_console.c tests/ui/test_logger.c \
                tests/app/test_static_variables.c \
                tests/app/test_etii_client.c tests/app/test_etii_server.c tests/app/test_app_runtime.c \
                tests/app/test_control_registry.c
TEST_SOLUTION16 := tests/core/test_solution16.c
# Jeu 256 (secondaire) : runner + suites communes. Jeu 16 (principal) : + solution16.
TEST_SRCS    := $(TEST_RUNNER) $(TEST_SUITES_COMMON)
TEST_SRCS_16 := $(TEST_RUNNER) $(TEST_SUITES_COMMON) $(TEST_SOLUTION16)

# Fixture « solution réelle » 4×4 : JSON figé (commité) -> tableau C (généré au
# build par un script Python, jamais commité). Cf. tests/fixtures/.
SOLUTION16_JSON := tests/fixtures/solution16.json
SOLUTION16_H    := tests/fixtures/solution16.h
GEN_SOLUTION16  := tests/fixtures/gen_solution16.py

$(SOLUTION16_H): $(SOLUTION16_JSON) $(GEN_SOLUTION16)
	python3 $(GEN_SOLUTION16) $(SOLUTION16_JSON) > $@
# Modules de production exercés + leurs dépendances de link transitives.
# tcpclient.c fournit le vrai create_tcp_client (plus de stub) ; tcpserver.c et
# local_socket.c sont désormais exercés directement (boucle locale / IPC AF_UNIX).
# NB : src/core/etii_search.c est ABSENT de cette liste à dessein —
# tests/core/test_etii_search.c l'inclut directement (#include "core/etii_search.c")
# pour tester ses helpers static ; le compiler aussi ici provoquerait des doubles
# symboles au link. Ce test est donc l'unique fournisseur des symboles etii_search.
TEST_MODULES := src/core/lifo.c src/core/part.c src/core/readdata.c src/ui/command_history.c src/ui/command_match.c src/core/possibility.c src/net/etii_protocol.c src/net/control_protocol.c src/core/datamanager.c src/net/local_socket.c src/net/tcpclient.c src/net/tcpserver.c src/ui/command_lines.c src/ui/console.c src/ui/logger.c src/app/static_variables.c src/app/etii_client.c src/app/etii_server.c src/app/control_registry.c src/app/app_runtime.c
# -Isrc : en-têtes de prod en "domaine/x.h". -Itests : greatest.h / fork_assert.h
# (harnais partagé à la racine de tests/, alors que les suites sont en sous-dossiers).
TEST_CFLAGS  := -Wall -std=gnu99 -O2 -g -Isrc -Itests
# Sanitizer optionnel pour `make test` : ASAN=1 instrumente le binaire de test
# avec AddressSanitizer (use-after-free, double-free, débordements de tas/pile).
# Utilisé par le job CI dédié. La détection de fuites (LSan) se règle au runtime
# via ASAN_OPTIONS (la CI la désactive : le code a des fuites connues hors du
# périmètre « sûreté mémoire »).
ASAN ?= 0
ifeq ($(ASAN),1)
    TEST_SANFLAGS := -fsanitize=address -fno-omit-frame-pointer
else
    TEST_SANFLAGS :=
endif

# `make test` = gate complet : binaire PRINCIPAL 16 (suites communes + solution16)
# puis binaire SECONDAIRE 256 (suites communes, chemins gardés #if ETERN_PARTS==256).
.PHONY: test test-16 test-256
test: test-16 test-256

# Binaire principal : ETERN_PARTS=16, où un plateau plein est exploitable.
test-16: $(SOLUTION16_H)
	gcc $(TEST_CFLAGS) -DETERN_PARTS=16 $(TEST_SANFLAGS) -pthread -o $(TEST_BIN_16) $(TEST_SRCS_16) $(TEST_MODULES) -lm
	./$(TEST_BIN_16)

# Binaire secondaire : build par défaut (256), suites communes uniquement.
test-256:
	gcc $(TEST_CFLAGS) $(TEST_SANFLAGS) -pthread -o $(TEST_BIN) $(TEST_SRCS) $(TEST_MODULES) -lm
	./$(TEST_BIN)

# ---------------------------------------------------------------------------
# Test d'intégration client/serveur (bout-en-bout) sur le puzzle 16 pièces.
#
# Compile un binaire dédié ETERN_PARTS=16 (le release nettoie build/ derrière
# lui : pas d'objets parasites pour un `make` 256 ultérieur), puis lance le
# scénario serveur+client de tests/integration/. Le client résout le 4×4,
# signale la solution ; le serveur l'affiche, sauvegarde son stock et s'arrête.
# Le script vérifie que les DEUX côtés ont le résultat, avec un timeout borné.
# INTEGRATION_TIMEOUT (défaut 60s) surcharge le délai max.
# ---------------------------------------------------------------------------
INTEGRATION_BIN     := eternityII16
INTEGRATION_TIMEOUT ?= 60
.PHONY: test-integration
test-integration:
	$(MAKE) CPPFLAGS="-DETERN_PARTS=16" EXECUTABLE=$(INTEGRATION_BIN)
	BIN=./$(INTEGRATION_BIN) DATA=data/pieces16.csv TIMEOUT=$(INTEGRATION_TIMEOUT) \
		bash tests/integration/run_solution_16.sh; \
		rc=$$?; rm -f ./$(INTEGRATION_BIN); exit $$rc

# ---------------------------------------------------------------------------
# Tests dans un conteneur Linux identique à la CI (`make test-docker`).
#
# Reproduit EN LOCAL l'environnement du workflow GitHub (Ubuntu + gcc + gcovr,
# image tests/docker/Dockerfile) : les écarts macOS/clang vs Linux/gcc
# (diagnostics -Werror, ASan, glibc, gcov) se voient AVANT le push. Le repo est
# monté en lecture seule sur /src puis copié dans /work (système de fichiers du
# conteneur) : les artefacts Linux (.o ELF, binaires) ne polluent jamais le
# répertoire de travail macOS, et réciproquement (`make clean` purge la copie).
# La séquence par défaut rejoue les jobs de test de la CI ; surchargeable :
#   make test-docker DOCKER_TEST_CMD="make test ASAN=1"
# ---------------------------------------------------------------------------
DOCKER          ?= docker
DOCKER_IMAGE    ?= eternityii-ci
DOCKER_TEST_CMD ?= make WERROR=1 && make test && make test ASAN=1 && make test-integration
.PHONY: test-docker
test-docker:
	$(DOCKER) build -t $(DOCKER_IMAGE) tests/docker
	$(DOCKER) run --rm \
		-v "$(CURDIR):/src:ro" \
		-e ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
		$(DOCKER_IMAGE) \
		bash -ce 'cp -R /src/. /work && make clean && $(DOCKER_TEST_CMD)'

# ---------------------------------------------------------------------------
# Couverture de code (gcov, intégré à gcc/clang — aucune install requise).
#
# Deux passes d'instrumentation (--coverage = -fprofile-arcs -ftest-coverage,
# -O0 pour un mapping ligne fidèle) : `coverage-256` (ETERN_PARTS=256) et
# `coverage-16` (ETERN_PARTS=16). Chacune compile un objet par source (.gcno
# aux noms propres) et confine ses artefacts dans son propre répertoire
# ($(COV_DIR) et $(COV_DIR_16)).
#
# `coverage` (= coverage-256 + coverage-16) fusionne les deux passes via gcovr
# --txt : l'union des lignes couvertes par les deux tailles de puzzle.
# Drill-down : $(COV_DIR)/<module>.c.gcov — lignes '#####' = jamais exécutées.
# ---------------------------------------------------------------------------
COV_DIR            := tests/coverage
COV_DIR_16         := tests/coverage-16
COV_CFLAGS         := -Wall -std=gnu99 -O0 -g -Isrc -Itests
# Couverture honnête sur TOUT le code de production : on instrumente chaque
# module du build par défaut (un .gcno par fichier). Ceux que les tests
# n'exercent pas (main.c, possibility.c, etii_*.c, …) n'ont pas de .gcda et
# ressortent donc à 0 % — le pourcentage global reflète l'ensemble du code.
# Exclus : logger_ncurses.c (variante NCURSES, exige <ncurses.h>) et
# gpu_pruner.cu (variante CUDA, exige nvcc) — absents du build standard.
COV_ALL_MODULES    := $(filter-out src/ui/logger_ncurses.c,$(wildcard src/*/*.c))
# Sous-ensemble réellement lié au binaire de test (le reste ne fournit qu'un
# .gcno → 0 %) : les modules exercés + leurs deps de link.
COV_LINK_MODULES   := $(TEST_MODULES)
# Modules dont la couverture provient d'un test qui les #include directement (et
# non d'un objet lié) : ils sont instrumentés AU TRAVERS de la TU de test, jamais
# compilés en standalone. Sinon leur .gcno standalone (sans .gcda — jamais lié)
# masquerait la vraie couverture dans le résumé `gcov`. gcovr, lui, fusionne par
# fichier source et les attribue correctement quoi qu'il arrive.
# Cf. tests/core/test_etii_search.c (#include "core/etii_search.c").
COV_INCLUDED_MODULES   := src/core/etii_search.c
COV_STANDALONE_MODULES := $(filter-out $(COV_INCLUDED_MODULES),$(COV_ALL_MODULES))

.PHONY: coverage-256
coverage-256:
	@mkdir -p $(COV_DIR)
	# 0. Purge les compteurs (.gcda) d'un run précédent : chaque `make coverage-256`
	#    repart de zéro. Indispensable depuis l'ajout des tests par fork (les
	#    fils flushent leur couverture en sortant ; re-fusionner sur des .gcda
	#    périmés provoque des avertissements « cannot merge / corrupt arc tag »).
	@rm -f $(COV_DIR)/*.gcda
	# 1. Instrumente les modules standalone + les sources de test → un .gcno par
	#    fichier (les modules jamais exécutés → 0 %). Les COV_INCLUDED_MODULES sont
	#    instrumentés via leur TU de test (TEST_SRCS), pas compilés seuls.
	@for src in $(COV_STANDALONE_MODULES) $(TEST_SRCS); do \
		gcc $(COV_CFLAGS) --coverage -pthread -c $$src \
			-o $(COV_DIR)/`basename $${src%.c}`.o || exit 1; \
	done
	# 2. Lie le binaire de test avec les seuls modules exercés (+ harnais) :
	#    main.c et les autres modules non liés ne produisent pas de .gcda.
	@gcc $(COV_CFLAGS) --coverage -pthread \
		$(addprefix $(COV_DIR)/,$(notdir $(TEST_SRCS:.c=.o))) \
		$(addprefix $(COV_DIR)/,$(notdir $(COV_LINK_MODULES:.c=.o))) \
		-lm -o $(COV_DIR)/run_tests
	@./$(COV_DIR)/run_tests
	@gcov -o $(COV_DIR) $(COV_STANDALONE_MODULES) >/dev/null 2>&1 || true
	# Les modules inclus par un test : leur .gcov réel vient de la TU de test.
	@gcov -o $(COV_DIR) $(TEST_SRCS) >/dev/null 2>&1 || true
	@mv -f *.gcov $(COV_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "===== Couverture de code (tout le code de production) ====="
	@for src in $(COV_STANDALONE_MODULES); do \
		line=`gcov -n -o $(COV_DIR) $$src 2>/dev/null \
			| grep -A1 "File '$$src'" | grep 'Lines executed' | head -1`; \
		printf "  %-18s %s\n" "$$src" "$${line:-Lines executed:0.00% (non exercé)}"; \
	done
	@for src in $(COV_INCLUDED_MODULES); do \
		line=`gcov -n -o $(COV_DIR) $(TEST_SRCS) 2>/dev/null \
			| grep -A1 "File '$$src'" | grep 'Lines executed' | head -1`; \
		printf "  %-18s %s\n" "$$src" "$${line:-Lines executed:0.00% (non exercé)}"; \
	done
	@echo "Détail annoté : $(COV_DIR)/<module>.c.gcov"

# Passe 16 : même instrumentation mais en -DETERN_PARTS=16, sources = jeu 16
# (suites communes + solution16). Artefacts confinés dans $(COV_DIR_16) pour que
# gcovr puisse fusionner cette passe avec la passe 256 (lignes #if-exclusives à
# chaque taille incluses dans l'union). Régénère d'abord le tableau C de la fixture.
.PHONY: coverage-16
coverage-16: $(SOLUTION16_H)

	@mkdir -p $(COV_DIR_16)
	@rm -f $(COV_DIR_16)/*.gcda
	@for src in $(COV_STANDALONE_MODULES) $(TEST_SRCS_16); do \
		gcc $(COV_CFLAGS) -DETERN_PARTS=16 --coverage -pthread -c $$src \
			-o $(COV_DIR_16)/`basename $${src%.c}`.o || exit 1; \
	done
	@gcc $(COV_CFLAGS) -DETERN_PARTS=16 --coverage -pthread \
		$(addprefix $(COV_DIR_16)/,$(notdir $(TEST_SRCS_16:.c=.o))) \
		$(addprefix $(COV_DIR_16)/,$(notdir $(COV_LINK_MODULES:.c=.o))) \
		-lm -o $(COV_DIR_16)/run_tests
	@./$(COV_DIR_16)/run_tests
	@gcov -o $(COV_DIR_16) $(COV_STANDALONE_MODULES) >/dev/null 2>&1 || true
	@gcov -o $(COV_DIR_16) $(TEST_SRCS_16) >/dev/null 2>&1 || true
	@mv -f *.gcov $(COV_DIR_16)/ 2>/dev/null || true
	@echo "Passe 16 instrumentée : $(COV_DIR_16)/"

# ---------------------------------------------------------------------------
# Rapports gcovr : Cobertura XML (Codecov), HTML navigable et résumé Markdown
# (Job Summary + commentaire de PR), en un seul passage.
#
# Réutilise les .gcda/.gcno produits par `coverage-256` et `coverage-16`.
# Nécessite gcovr (pip install gcovr ou pipx ; bien plus léger/rapide que lcov).
# On exclut le répertoire des tests pour ne reporter que le code de production ;
# sur macOS gcov natif = llvm-cov, on le signale à gcovr.
# ---------------------------------------------------------------------------
COV_XML    := $(COV_DIR)/coverage.xml
COV_HTML   := $(COV_DIR)/html
COV_MD     := $(COV_DIR)/coverage.md
COV_JSON   := $(COV_DIR)/coverage-summary.json
COV_FILTER := --exclude '(^|/)tests/'
# Post-traitement : insère une section « Couverture par domaine » dans COV_MD.
COV_BY_DOMAIN := tests/coverage_by_domain.py
GCOVR := gcovr
ifeq ($(detected_OS),Darwin)
	GCOVR := gcovr --gcov-executable "$(shell xcrun --find llvm-cov) gcov"
endif

# Fusionne les DEUX passes (256 + 16) : gcovr agrège les compteurs par ligne
# source sur les deux dossiers -> résumé texte = union des lignes couvertes.
# Point d'entrée habituel pour un check rapide en local (nécessite gcovr).
.PHONY: coverage
coverage: coverage-256 coverage-16
	$(GCOVR) --root . $(COV_FILTER) $(COV_DIR) $(COV_DIR_16) --txt

# Génère en plus le XML Cobertura, le rapport HTML et le résumé Markdown.
.PHONY: coverage-report
coverage-report: coverage
	@mkdir -p $(COV_HTML)
	$(GCOVR) --root . $(COV_FILTER) $(COV_DIR) $(COV_DIR_16) \
		--txt \
		--cobertura $(COV_XML) \
		--html-details $(COV_HTML)/index.html \
		--json-summary $(COV_JSON) \
		--markdown $(COV_MD) --markdown-title "Couverture de code"
	@python3 $(COV_BY_DOMAIN) $(COV_JSON) $(COV_MD) $(COV_XML)
	@echo "Cobertura : $(COV_XML)"
	@echo "HTML      : $(COV_HTML)/index.html"
	@echo "Markdown  : $(COV_MD)  (avec section par domaine)"
