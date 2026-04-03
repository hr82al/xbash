/*
 * readline_filter_history_sqlite.c -- SQLite3-backed substring history search.
 *
 * READLINE_INPUT_FILTER_LIB plugin that replaces Up/Down arrow behavior:
 *   - Searches history by SUBSTRING (any part of the line), not just prefix
 *   - Each press of Up finds the next matching entry
 *   - History stored in ~/.config/bash/history.sql (SQLite3)
 *   - Auto-imports ~/.bash_history on first run
 *   - Deduplicates: repeated commands update timestamp (most recent first)
 *
 * Build:
 *   gcc -shared -fPIC -O2 -o history_sqlite.so \
 *       readline_filter_history_sqlite.c -lsqlite3
 *
 * Usage:
 *   export READLINE_INPUT_FILTER_LIB=/path/to/history_sqlite.so
 *   bash
 *
 * Hot-reload:
 *   READLINE_INPUT_FILTER_LIB=/path/to/history_sqlite.so
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define DB_DIR_RELATIVE  "/.config/bash"
#define DB_NAME          "/history.sql"
#define BASH_HISTORY     "/.bash_history"
#define MAX_LINE         8192

/* Key sequences */
#define IS_UP_ARROW(seq, len)   ((len) == 3 && (seq)[0] == '\x1b' && (seq)[1] == '[' && (seq)[2] == 'A')
#define IS_DOWN_ARROW(seq, len) ((len) == 3 && (seq)[0] == '\x1b' && (seq)[1] == '[' && (seq)[2] == 'B')
#define IS_ENTER(seq, len)      ((len) == 1 && (seq)[0] == '\r')

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static sqlite3 *db;
static sqlite3_stmt *stmt_search;
static sqlite3_stmt *stmt_insert;

static char saved_line[MAX_LINE];   /* line before navigation started */
static char query[MAX_LINE];        /* search query (what user typed) */
static int  search_offset;          /* current result offset */
static int  navigating;             /* nonzero while cycling with arrows */

/* ------------------------------------------------------------------ */
/* Database setup                                                      */
/* ------------------------------------------------------------------ */

static char *
get_db_path (void)
{
  const char *home;
  char *path;
  int len;

  home = getenv ("HOME");
  if (!home || !*home)
    home = "/tmp";

  len = strlen (home) + strlen (DB_DIR_RELATIVE) + strlen (DB_NAME) + 1;
  path = malloc (len);
  if (!path)
    return NULL;

  snprintf (path, len, "%s%s%s", home, DB_DIR_RELATIVE, DB_NAME);
  return path;
}

static int
ensure_dir (const char *path)
{
  /* Extract directory part and create it recursively. */
  char *dir, *p;

  dir = strdup (path);
  if (!dir)
    return -1;

  /* Find last '/' */
  p = strrchr (dir, '/');
  if (p)
    *p = '\0';

  /* mkdir -p: create each component */
  for (p = dir + 1; *p; p++)
    {
      if (*p == '/')
	{
	  *p = '\0';
	  mkdir (dir, 0755);
	  *p = '/';
	}
    }
  mkdir (dir, 0755);

  free (dir);
  return 0;
}

static int
create_schema (void)
{
  const char *sql =
    "CREATE TABLE IF NOT EXISTS history ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  command   TEXT    NOT NULL UNIQUE,"
    "  timestamp INTEGER DEFAULT (strftime('%s', 'now'))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_history_ts ON history(timestamp);";

  return sqlite3_exec (db, sql, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Import ~/.bash_history                                              */
/* ------------------------------------------------------------------ */

static void
import_bash_history (void)
{
  const char *home;
  char path[4096];
  FILE *fp;
  char line[MAX_LINE];
  long ts = 0;
  int has_ts = 0;
  int count = 0;

  home = getenv ("HOME");
  if (!home)
    return;
  snprintf (path, sizeof (path), "%s%s", home, BASH_HISTORY);

  fp = fopen (path, "r");
  if (!fp)
    return;

  sqlite3_exec (db, "BEGIN TRANSACTION", NULL, NULL, NULL);

  while (fgets (line, sizeof (line), fp))
    {
      /* Strip trailing newline. */
      int len = strlen (line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
	line[--len] = '\0';

      if (len == 0)
	continue;

      /* Timestamp lines start with '#' followed by digits. */
      if (line[0] == '#')
	{
	  char *endp;
	  long val = strtol (line + 1, &endp, 10);
	  if (endp != line + 1 && (*endp == '\0' || *endp == '\n'))
	    {
	      ts = val;
	      has_ts = 1;
	      continue;
	    }
	}

      /* Insert the command. */
      {
	sqlite3_stmt *s;
	const char *insert_sql = has_ts
	  ? "INSERT OR IGNORE INTO history (command, timestamp) VALUES (?, ?)"
	  : "INSERT OR IGNORE INTO history (command) VALUES (?)";

	if (sqlite3_prepare_v2 (db, insert_sql, -1, &s, NULL) == SQLITE_OK)
	  {
	    sqlite3_bind_text (s, 1, line, -1, SQLITE_TRANSIENT);
	    if (has_ts)
	      sqlite3_bind_int64 (s, 2, ts);
	    sqlite3_step (s);
	    sqlite3_finalize (s);
	    count++;
	  }
      }

      has_ts = 0;
      ts = 0;
    }

  sqlite3_exec (db, "COMMIT", NULL, NULL, NULL);
  fclose (fp);

  if (count > 0)
    fprintf (stderr, "bash: history_sqlite: imported %d entries from %s\n",
	     count, path);
}

/* ------------------------------------------------------------------ */
/* Prepared statements                                                 */
/* ------------------------------------------------------------------ */

static int
prepare_statements (void)
{
  const char *search_sql =
    "SELECT command FROM history "
    "WHERE command LIKE '%' || ?1 || '%' "
    "  AND command != ?1 "
    "ORDER BY timestamp DESC "
    "LIMIT 1 OFFSET ?2";

  const char *insert_sql =
    "INSERT INTO history (command) VALUES (?1) "
    "ON CONFLICT(command) DO UPDATE SET timestamp = strftime('%s', 'now')";

  if (sqlite3_prepare_v2 (db, search_sql, -1, &stmt_search, NULL) != SQLITE_OK)
    return -1;
  if (sqlite3_prepare_v2 (db, insert_sql, -1, &stmt_insert, NULL) != SQLITE_OK)
    return -1;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Search helper                                                       */
/* ------------------------------------------------------------------ */

/* Search history for entries containing `pattern` as substring.
   Returns the entry at the given offset (0 = most recent match),
   or NULL if no more matches.  The returned string is valid until
   the next call. */
static const char *
search_history (const char *pattern, int offset)
{
  const char *result = NULL;

  sqlite3_reset (stmt_search);
  sqlite3_bind_text (stmt_search, 1, pattern, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (stmt_search, 2, offset);

  if (sqlite3_step (stmt_search) == SQLITE_ROW)
    result = (const char *)sqlite3_column_text (stmt_search, 0);

  return result;
}

/* Save a command to history. */
static void
save_command (const char *cmd)
{
  if (!cmd || !*cmd)
    return;

  /* Skip whitespace-only commands. */
  {
    const char *p = cmd;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0')
      return;
  }

  sqlite3_reset (stmt_insert);
  sqlite3_bind_text (stmt_insert, 1, cmd, -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt_insert);
}

/* ------------------------------------------------------------------ */
/* Copy result into out_line buffer.                                   */
/* ------------------------------------------------------------------ */

static int
write_result (const char *text, char *out_line, int out_line_size,
	      int *out_point, int *out_mark)
{
  int len;

  if (!text)
    return 0;

  len = strlen (text);
  if (len >= out_line_size)
    len = out_line_size - 1;

  memcpy (out_line, text, len);
  out_line[len] = '\0';
  *out_point = len;
  *out_mark = 0;

  return 1;
}

/* ------------------------------------------------------------------ */
/* READLINE_INPUT_FILTER_LIB API                                       */
/* ------------------------------------------------------------------ */

int
readline_filter_init (void)
{
  char *db_path;
  int is_empty;

  db_path = get_db_path ();
  if (!db_path)
    return -1;

  ensure_dir (db_path);

  if (sqlite3_open (db_path, &db) != SQLITE_OK)
    {
      fprintf (stderr, "bash: history_sqlite: cannot open %s: %s\n",
	       db_path, sqlite3_errmsg (db));
      free (db_path);
      return -1;
    }

  free (db_path);

  /* Performance: WAL mode + reduced sync for interactive use. */
  sqlite3_exec (db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
  sqlite3_exec (db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

  if (create_schema () != SQLITE_OK)
    {
      fprintf (stderr, "bash: history_sqlite: schema error: %s\n",
	       sqlite3_errmsg (db));
      sqlite3_close (db);
      db = NULL;
      return -1;
    }

  /* Check if history table is empty → import ~/.bash_history. */
  {
    sqlite3_stmt *s;
    is_empty = 1;
    if (sqlite3_prepare_v2 (db, "SELECT 1 FROM history LIMIT 1", -1, &s, NULL) == SQLITE_OK)
      {
	if (sqlite3_step (s) == SQLITE_ROW)
	  is_empty = 0;
	sqlite3_finalize (s);
      }
  }

  if (is_empty)
    import_bash_history ();

  if (prepare_statements () < 0)
    {
      fprintf (stderr, "bash: history_sqlite: prepare error: %s\n",
	       sqlite3_errmsg (db));
      sqlite3_close (db);
      db = NULL;
      return -1;
    }

  navigating = 0;
  search_offset = 0;
  saved_line[0] = '\0';
  query[0] = '\0';

  return 0;
}

void
readline_filter_cleanup (void)
{
  if (stmt_search)
    { sqlite3_finalize (stmt_search); stmt_search = NULL; }
  if (stmt_insert)
    { sqlite3_finalize (stmt_insert); stmt_insert = NULL; }
  if (db)
    { sqlite3_close (db); db = NULL; }
}

int
readline_filter_keypress (const char *keyseq, int keyseq_len,
			  const char *line, int line_len,
			  int point, int mark,
			  char *out_line, int out_line_size,
			  int *out_point, int *out_mark)
{
  const char *result;

  if (!db)
    return 0;

  /* --- Up arrow: search backward --- */
  if (IS_UP_ARROW (keyseq, keyseq_len))
    {
      if (!navigating)
	{
	  /* Start new search: save current line as query. */
	  navigating = 1;
	  search_offset = 0;
	  if (line_len < (int)sizeof (saved_line))
	    { memcpy (saved_line, line, line_len); saved_line[line_len] = '\0'; }
	  else
	    saved_line[0] = '\0';
	  if (line_len < (int)sizeof (query))
	    { memcpy (query, line, line_len); query[line_len] = '\0'; }
	  else
	    query[0] = '\0';
	}

      result = search_history (query, search_offset);
      if (result)
	{
	  search_offset++;
	  return write_result (result, out_line, out_line_size, out_point, out_mark);
	}
      /* No more matches — stay on the last one. */
      return 0;
    }

  /* --- Down arrow: search forward --- */
  if (IS_DOWN_ARROW (keyseq, keyseq_len))
    {
      if (!navigating)
	return 0;

      search_offset--;
      if (search_offset < 0)
	{
	  /* Past the beginning — restore original line. */
	  navigating = 0;
	  search_offset = 0;
	  return write_result (saved_line, out_line, out_line_size, out_point, out_mark);
	}

      result = search_history (query, search_offset);
      if (result)
	return write_result (result, out_line, out_line_size, out_point, out_mark);

      /* Shouldn't happen, but restore saved line as fallback. */
      navigating = 0;
      search_offset = 0;
      return write_result (saved_line, out_line, out_line_size, out_point, out_mark);
    }

  /* --- Enter: save command to database --- */
  if (IS_ENTER (keyseq, keyseq_len))
    {
      /* Save the line that will be executed (may differ from original query
	 if user navigated and accepted a history entry). */
      if (line_len > 0)
	{
	  char cmd[MAX_LINE];
	  int len = (line_len < (int)sizeof (cmd) - 1) ? line_len : (int)sizeof (cmd) - 1;
	  memcpy (cmd, line, len);
	  cmd[len] = '\0';
	  save_command (cmd);
	}
      navigating = 0;
      search_offset = 0;
      return 0;
    }

  /* --- Any other key: stop navigating --- */
  if (navigating)
    {
      navigating = 0;
      search_offset = 0;
    }

  return 0;
}
