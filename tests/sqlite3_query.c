/* Minimal sqlite3 query tool for testing (when sqlite3 CLI is not installed).
   Usage: sqlite3_query <db_path> <sql> */

#include <stdio.h>
#include <sqlite3.h>

static int callback(void *unused, int argc, char **argv, char **colnames) {
  for (int i = 0; i < argc; i++) {
    if (i > 0) printf("|");
    printf("%s", argv[i] ? argv[i] : "NULL");
  }
  printf("\n");
  return 0;
}

int main(int argc, char **argv) {
  sqlite3 *db;
  char *err = NULL;
  if (argc != 3) { fprintf(stderr, "Usage: %s <db> <sql>\n", argv[0]); return 1; }
  if (sqlite3_open(argv[1], &db) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(db)); return 1; }
  if (sqlite3_exec(db, argv[2], callback, NULL, &err) != SQLITE_OK) { fprintf(stderr, "%s\n", err); sqlite3_free(err); sqlite3_close(db); return 1; }
  sqlite3_close(db);
  return 0;
}
