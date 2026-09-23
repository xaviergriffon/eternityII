/*
 * Sonde d'arènes malloc : le binaire de test se relance lui-même avec
 * ARENA_PROBE_ENV pour observer le plafond d'arènes du serveur dans un process
 * neuf (cf. server_arena_cap_keeps_connection_threads_in_one_arena,
 * tests/app/test_app_runtime.c).
 */
#ifndef ETII_TEST_ARENA_PROBE_H
#define ETII_TEST_ARENA_PROBE_H

#define ARENA_PROBE_ENV "ETII_TEST_ARENA_PROBE"

/// Code de sortie = nombre d'arènes malloc vues avec 8 threads vivants.
int app_runtime_arena_probe(const char *mode);

#endif
