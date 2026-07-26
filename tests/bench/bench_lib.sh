#!/usr/bin/env bash
#
# Fonctions PURES (aucune E/S, aucun état global) partagées par le banc de
# mesure `tests/bench/bench_search.sh` et testées par
# `tests/bench/test_bench_parse.sh`.
#
# Même principe que côté C (cf. `bench_should_stop` / `bench_parse_nodes_env`
# dans src/app/static_variables.c) : la logique qu'on veut verrouiller par un
# test est isolée dans une fonction sans effet de bord, plutôt que noyée dans
# un script qui compile, lance des process et mesure.
#
# Ce fichier est destiné à être `source`é ; il ne fait rien à l'exécution.

# ---------------------------------------------------------------------------
# bench_parse_elapsed <texte>
#
# Normalise et VALIDE un temps écoulé tel que produit par `time` avec
# TIMEFORMAT='%R'. Écrit la valeur normalisée (espaces retirés) sur stdout et
# renvoie 0 si c'est un décimal strictement positif ; n'écrit rien et renvoie 1
# sinon.
#
# Pourquoi valider, et pas seulement tester la non-vacuité comme avant : **bash
# lui-même peut imprimer un temps malformé**. Sa conversion µs → ms
# (`timeval_to_secs`, execute_cmd.c) arrondit sans propager la retenue vers les
# secondes : dès 999500 µs la fraction vaut 1000, et `mkfmt` imprime chaque
# chiffre par `(fraction / 100) + '0'`, soit 10 + '0' = ':'. Un run de 2,9997 s
# est donc rapporté « 2.:00 » au lieu de « 3.000 ». Reproduit sur bash 5.3 :
#
#     TIMEFORMAT='%R'; { time ./spin 995200us ; } 2> tf   # -> "0.:00"
#
# La fenêtre est étroite (500 µs par seconde, ~0,05 % des runs) mais les
# conséquences ne le sont pas : awk lit « 2.:00 » comme 2.0, le débit du run est
# surestimé (+50 % dans le cas observé : 10,0 M nœuds/s au lieu de 6,7 M), cette
# valeur devient le `nodes_per_sec_max` du rapport et gonfle l'écart-type relatif
# (17,55 % au lieu de ~1,6 %) — et le JSON produit (`"elapsed_sec":2.:00`) n'est
# même plus parsable. La médiane absorbe l'incident à --reps 5, plus forcément à
# --reps 3.
#
# Le refus du zéro (`0`, `0.000`) évite l'autre silence possible : le calcul de
# débit retombe sur `(t > 0) ? n / t : 0`, soit un débit de 0 rapporté comme une
# mesure valide.
# ---------------------------------------------------------------------------
bench_parse_elapsed() {
    local v="${1//[[:space:]]/}"
    # Décimal strict : ni signe, ni exposant, ni unité collée, ni séparateur
    # autre que le point (le script force LC_ALL=C, la virgule est une erreur).
    [[ "$v" =~ ^[0-9]+(\.[0-9]+)?$ ]] || return 1
    # Au moins un chiffre non nul : 0.000 n'est pas un temps de run mesurable.
    [[ "$v" == *[1-9]* ]] || return 1
    printf '%s' "$v"
}

# ---------------------------------------------------------------------------
# bench_retry_valid_time <tentatives> <fn_log> <libellé> <fn_run>
#
# Appelle <fn_run> — qui exécute un run et écrit sur SA sortie standard le temps
# brut produit par `time` — jusqu'à en obtenir un temps valide au sens de
# `bench_parse_elapsed`. Écrit ce temps validé sur stdout et renvoie 0 ; renvoie
# 1 après <tentatives> échecs, sans rien écrire sur stdout.
#
# Rejouer plutôt qu'échouer d'emblée : la cause connue (bogue d'arrondi de bash,
# voir bench_parse_elapsed) dépend de la durée mesurée à 500 µs près, donc une
# nouvelle exécution retombe presque certainement sur une valeur correcte. Et
# rejouer un run coûte quelques secondes, là où l'ancien comportement — accepter
# la valeur telle quelle — coûtait un rapport faux et silencieux.
#
# La borne évite la boucle infinie si la cause est au contraire systématique
# (message du shell atterrissant dans le fichier de temps, TIMEFORMAT écrasé,
# `time` externe au lieu du mot-clé…) : dans ce cas le banc s'arrête, plutôt que
# de publier un débit faux.
#
# <fn_log> et <fn_run> sont passées par NOM (le script y injecte ses propres
# `log`/`run_once`, le test des doublures) : c'est ce qui rend cette boucle
# testable sans compiler ni lancer le solveur.
# ---------------------------------------------------------------------------
bench_retry_valid_time() {
    local attempts="$1" fn_log="$2" label="$3" fn_run="$4"
    local i raw elapsed

    for ((i = 1; i <= attempts; i++)); do
        raw=$("$fn_run")
        if elapsed=$(bench_parse_elapsed "$raw"); then
            printf '%s' "$elapsed"
            return 0
        fi
        if (( i < attempts )); then
            "$fn_log" "$label : temps écoulé illisible (« ${raw//$'\n'/ } ») —" \
                "tentative $i/$attempts, on rejoue le run."
        else
            "$fn_log" "$label : temps écoulé illisible (« ${raw//$'\n'/ } ») —" \
                "tentative $i/$attempts, dernière."
        fi
    done

    "$fn_log" "$label : temps écoulé toujours illisible après $attempts tentative(s)" \
        "— banc interrompu plutôt que de rapporter un débit faux."
    return 1
}
