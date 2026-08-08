# Image de test locale calée sur le job CI (`.github/workflows/ci.yml`,
# runs-on: ubuntu-latest) : même distro, même chaîne gcc/make/gcov, gcovr via
# pipx comme en CI. Sert à reproduire EN LOCAL (macOS/clang) les échecs
# spécifiques à Linux/gcc — via `make test-docker`, jamais poussée en registre.
#
# Épinglé sur ubuntu:24.04 (= ubuntu-latest actuel) plutôt que :latest, pour
# que le local ne dérive pas silencieusement de la CI. À réviser quand GitHub
# migrera ubuntu-latest.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# build-essential : gcc + make (+ gcov, livré avec gcc), comme préinstallés
#                   sur le runner GitHub.
# python3         : gen_solution16.py (fixture des tests) et
#                   coverage_by_domain.py.
# pipx + ca-certificates : installation de gcovr comme en CI (`pipx install`).
# procps          : pkill, requis par tests/integration/run_solution_16.sh
#                   et tests/integration/run_control_channel.sh.
# crossbuild-essential-arm64 : gcc-aarch64-linux-gnu + libc6-dev-arm64-cross,
#                   pour `make test-docker-arm` (compile-check du build ARM
#                   64-bit d'un Raspberry Pi sans matériel ARM ni QEMU).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        python3 \
        pipx \
        ca-certificates \
        procps \
        crossbuild-essential-arm64 \
    && rm -rf /var/lib/apt/lists/*

# gcovr comme en CI (étape « Dépendance gcovr » du workflow).
RUN pipx install gcovr
ENV PATH="/root/.local/bin:${PATH}"

# Le repo hôte est monté en lecture seule sur /src ; la cible `test-docker`
# le copie vers /work (overlayfs du conteneur) avant de compiler, pour que
# les artefacts Linux (.o ELF, binaires, .gcda) ne se mélangent jamais aux
# artefacts macOS (Mach-O) du répertoire de travail hôte.
WORKDIR /work

CMD ["bash"]
