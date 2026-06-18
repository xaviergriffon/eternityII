# Transforme un rapport lcov (.info) en tableau Markdown de couverture.
# Usage : awk -f tests/lcov_to_md.awk tests/coverage/coverage.info
#
# Sortie : un tableau « Fichier | Lignes | Fonctions » (une ligne par SF,
# pourcentage + hits/total), suivi d'une ligne Total agrégée. Conçu pour être
# injecté dans $GITHUB_STEP_SUMMARY et dans un commentaire de PR.

function pct(hit, total) {
    if (total + 0 == 0) return "n/a"
    return sprintf("%.1f%% (%d/%d)", 100.0 * hit / total, hit, total)
}

BEGIN {
    print "## Couverture de code"
    print ""
    print "| Fichier | Lignes | Fonctions |"
    print "| --- | --- | --- |"
}

/^SF:/   { file = substr($0, 4); n = split(file, a, "/"); name = a[n]
           lf = lh = fnf = fnh = 0 }
/^LF:/   { lf  = substr($0, 4) }
/^LH:/   { lh  = substr($0, 4) }
/^FNF:/  { fnf = substr($0, 5) }
/^FNH:/  { fnh = substr($0, 5) }
/^end_of_record/ {
    tlf += lf; tlh += lh; tfnf += fnf; tfnh += fnh
    printf "| %s | %s | %s |\n", name, pct(lh, lf), pct(fnh, fnf)
}

END {
    printf "| **Total** | **%s** | **%s** |\n", pct(tlh, tlf), pct(tfnh, tfnf)
}
