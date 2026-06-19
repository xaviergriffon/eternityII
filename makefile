ifeq ($(OS),Windows_NT) 
    detected_OS := Windows
else
    detected_OS := $(shell sh -c 'uname 2>/dev/null || echo Unknown')
endif


OPENCLLIB := -lOpenCL
ifeq ($(detected_OS),Darwin)
	OPENCLLIB := -framework OpenCL
endif

# etii_opencl.o ${OPENCLLIB}

# Ajout d'une variable DEBUG pour activer ou désactiver les informations de débogage
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS= -Wall -std=gnu99 -Ofast -g
	CLEAN_OBJS =
else
    CFLAGS= -Wall -std=gnu99 -Ofast
	CLEAN_OBJS = rm *.o
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
    CFLAGS += -DWITH_CUDA
    CUDA_OBJ := gpu_pruner.o
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

$(EXECUTABLE): $(LOGGER_OBJ) static_variables.o local_socket.o lifo.o tcpclient.o tcpserver.o part.o readdata.o datamanager.o possibility.o etii_protocol.o etii_client.o etii_server.o etii_search.o command_lines.o command_match.o command_history.o console.o main.o $(CUDA_OBJ)
	gcc -pthread -o $(EXECUTABLE) $(LOGGER_OBJ) static_variables.o local_socket.o lifo.o tcpclient.o tcpserver.o part.o datamanager.o possibility.o readdata.o etii_protocol.o etii_client.o etii_server.o etii_search.o command_lines.o command_match.o command_history.o console.o main.o $(CUDA_OBJ) ${CFLAGS} ${CPPFLAGS} $(NCURSES_LIB) $(CUDA_LIB)
	$(CLEAN_OBJS)

# Compilation du module GPU (uniquement requis lorsque CUDA=1 ; cette règle n'est
# jamais invoquée sans CUDA=1 car gpu_pruner.o n'est alors pas une dépendance).
gpu_pruner.o: gpu_pruner.cu gpu_pruner.h possibility.h part.h static_variables.h logger.h
	$(NVCC) $(NVCCFLAGS) -DWITH_CUDA -c gpu_pruner.cu -o gpu_pruner.o

clean:
	rm -f *.o $(EXECUTABLE)
	rm -f $(TEST_BIN)
	rm -rf $(COV_DIR)
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
TEST_SRCS    := tests/test_main.c tests/test_lifo.c tests/test_part.c tests/test_readdata.c tests/test_command_history.c tests/test_possibility.c tests/test_etii_protocol.c tests/test_command_match.c tests/test_datamanager.c tests/stubs.c
# Modules de production exercés + leurs dépendances de link transitives.
# tests/stubs.c fournit create_tcp_client (no-op) : seul symbole réseau réclamé
# par datamanager.c une fois les autres modules liés.
TEST_MODULES := lifo.c part.c readdata.c command_history.c command_match.c possibility.c etii_protocol.c datamanager.c logger.c static_variables.c
TEST_CFLAGS  := -Wall -std=gnu99 -O2 -g -I.

.PHONY: test
test:
	gcc $(TEST_CFLAGS) -pthread -o $(TEST_BIN) $(TEST_SRCS) $(TEST_MODULES) -lm
	./$(TEST_BIN)

# ---------------------------------------------------------------------------
# Couverture de code (gcov, intégré à gcc/clang — aucune install requise).
#
# Compile la suite instrumentée (--coverage = -fprofile-arcs -ftest-coverage,
# -O0 pour un mapping ligne fidèle), la lance, puis agrège la couverture des
# modules réellement testés. On compile un objet par source (.gcno aux noms
# propres) pour que `gcov` s'y retrouve. Tous les artefacts (.o/.gcno/.gcda et
# les .gcov annotés) restent confinés dans $(COV_DIR).
#
# Drill-down : ouvrir $(COV_DIR)/<module>.c.gcov — les lignes jamais exécutées
# sont marquées '#####'. Rapport HTML optionnel via lcov : voir tests/README.md.
# ---------------------------------------------------------------------------
COV_DIR            := tests/coverage
COV_CFLAGS         := -Wall -std=gnu99 -O0 -g -I.
# Couverture honnête sur TOUT le code de production : on instrumente chaque
# module du build par défaut (un .gcno par fichier). Ceux que les tests
# n'exercent pas (main.c, possibility.c, etii_*.c, …) n'ont pas de .gcda et
# ressortent donc à 0 % — le pourcentage global reflète l'ensemble du code.
# Exclus : logger_ncurses.c (variante NCURSES, exige <ncurses.h>) et
# gpu_pruner.cu (variante CUDA, exige nvcc) — absents du build standard.
COV_ALL_MODULES    := $(filter-out logger_ncurses.c,$(wildcard *.c))
# Sous-ensemble réellement lié au binaire de test (le reste ne fournit qu'un
# .gcno → 0 %) : les modules exercés + leurs deps de link.
COV_LINK_MODULES   := $(TEST_MODULES)

.PHONY: coverage
coverage:
	@mkdir -p $(COV_DIR)
	# 0. Purge les compteurs (.gcda) d'un run précédent : chaque `make coverage`
	#    repart de zéro. Indispensable depuis l'ajout des tests par fork (les
	#    fils flushent leur couverture en sortant ; re-fusionner sur des .gcda
	#    périmés provoque des avertissements « cannot merge / corrupt arc tag »).
	@rm -f $(COV_DIR)/*.gcda
	# 1. Instrumente TOUS les modules de production + les sources de test → un
	#    .gcno par fichier, y compris les modules jamais exécutés (→ 0 %).
	@for src in $(COV_ALL_MODULES) $(TEST_SRCS); do \
		gcc $(COV_CFLAGS) --coverage -pthread -c $$src \
			-o $(COV_DIR)/`basename $${src%.c}`.o || exit 1; \
	done
	# 2. Lie le binaire de test avec les seuls modules exercés (+ harnais) :
	#    main.c et les autres modules non liés ne produisent pas de .gcda.
	@gcc $(COV_CFLAGS) --coverage -pthread \
		$(addprefix $(COV_DIR)/,$(notdir $(TEST_SRCS:.c=.o))) \
		$(addprefix $(COV_DIR)/,$(COV_LINK_MODULES:.c=.o)) \
		-lm -o $(COV_DIR)/run_tests
	@./$(COV_DIR)/run_tests
	@gcov -o $(COV_DIR) $(COV_ALL_MODULES) >/dev/null 2>&1 || true
	@mv -f *.gcov $(COV_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "===== Couverture de code (tout le code de production) ====="
	@for src in $(COV_ALL_MODULES); do \
		line=`gcov -n -o $(COV_DIR) $$src 2>/dev/null \
			| grep -A1 "File '$$src'" | grep 'Lines executed' | head -1`; \
		printf "  %-18s %s\n" "$$src" "$${line:-Lines executed:0.00% (non exercé)}"; \
	done
	@echo "Détail annoté : $(COV_DIR)/<module>.c.gcov"

# ---------------------------------------------------------------------------
# Rapports gcovr : Cobertura XML (Codecov), HTML navigable et résumé Markdown
# (Job Summary + commentaire de PR), en un seul passage.
#
# Réutilise les .gcda/.gcno produits par `coverage`. Nécessite gcovr (pip install
# gcovr ou pipx ; bien plus léger/rapide que lcov). On exclut le répertoire des
# tests pour ne reporter que le code de production ; sur macOS gcov natif =
# llvm-cov, on le signale à gcovr.
# ---------------------------------------------------------------------------
COV_XML    := $(COV_DIR)/coverage.xml
COV_HTML   := $(COV_DIR)/html
COV_MD     := $(COV_DIR)/coverage.md
COV_FILTER := --exclude '(^|/)tests/'
GCOVR := gcovr
ifeq ($(detected_OS),Darwin)
	GCOVR := gcovr --gcov-executable "$(shell xcrun --find llvm-cov) gcov"
endif

.PHONY: coverage-report
coverage-report: coverage
	@mkdir -p $(COV_HTML)
	$(GCOVR) --root . $(COV_FILTER) $(COV_DIR) \
		--txt \
		--cobertura $(COV_XML) \
		--html-details $(COV_HTML)/index.html \
		--markdown $(COV_MD) --markdown-title "Couverture de code"
	@echo "Cobertura : $(COV_XML)"
	@echo "HTML      : $(COV_HTML)/index.html"
	@echo "Markdown  : $(COV_MD)"
