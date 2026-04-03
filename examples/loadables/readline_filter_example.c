/*
 * readline_filter_example.c -- Example READLINE_INPUT_FILTER_LIB plugin in C.
 *
 * Demonstrates all four optional API functions:
 *   - readline_filter_init / readline_filter_cleanup (lifecycle)
 *   - readline_filter_keypress (auto-uppercase + custom Up/Down history)
 *   - readline_filter_completions (custom completions for words starting with @)
 *   - readline_filter_display_matches (bulleted completion list)
 *
 * Build:
 *   gcc -shared -fPIC -o readline_filter_example.so readline_filter_example.c
 *
 * Usage:
 *   export READLINE_INPUT_FILTER_LIB=./readline_filter_example.so
 *   bash
 *
 * Hot-reload after recompilation:
 *   READLINE_INPUT_FILTER_LIB=./readline_filter_example.so
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Simple history ring buffer for demo. */
#define MAX_HISTORY 256
static char *history[MAX_HISTORY];
static int history_count = 0;
static int history_pos = -1;
static char saved_line[8192];

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

int
readline_filter_init (void)
{
  memset (history, 0, sizeof (history));
  history_count = 0;
  history_pos = -1;
  saved_line[0] = '\0';
  return 0;
}

void
readline_filter_cleanup (void)
{
  int i;
  for (i = 0; i < history_count; i++)
    free (history[i]);
  history_count = 0;
}

/* ------------------------------------------------------------------ */
/* Post-keystroke hook                                                */
/* ------------------------------------------------------------------ */

int
readline_filter_keypress (const char *keyseq, int keyseq_len,
			  const char *line, int line_len,
			  int point, int mark,
			  char *out_line, int out_line_size,
			  int *out_point, int *out_mark)
{
  int i;

  /* Detect Up arrow: ESC [ A */
  if (keyseq_len == 3 && keyseq[0] == '\x1b' && keyseq[1] == '[' && keyseq[2] == 'A')
    {
      if (history_pos == -1 && history_count > 0)
	{
	  /* Save current line before navigating. */
	  if (line_len < (int)sizeof (saved_line))
	    {
	      memcpy (saved_line, line, line_len);
	      saved_line[line_len] = '\0';
	    }
	  history_pos = history_count - 1;
	}
      else if (history_pos > 0)
	history_pos--;
      else
	return 0;

      if (history_pos >= 0 && history_pos < history_count)
	{
	  int len = strlen (history[history_pos]);
	  if (len >= out_line_size) len = out_line_size - 1;
	  memcpy (out_line, history[history_pos], len);
	  out_line[len] = '\0';
	  *out_point = len;
	  *out_mark = 0;
	  return 1;
	}
      return 0;
    }

  /* Detect Down arrow: ESC [ B */
  if (keyseq_len == 3 && keyseq[0] == '\x1b' && keyseq[1] == '[' && keyseq[2] == 'B')
    {
      if (history_pos >= 0)
	{
	  history_pos++;
	  if (history_pos >= history_count)
	    {
	      /* Past end — restore saved line. */
	      history_pos = -1;
	      strcpy (out_line, saved_line);
	      *out_point = strlen (saved_line);
	      *out_mark = 0;
	      return 1;
	    }
	  else
	    {
	      int len = strlen (history[history_pos]);
	      if (len >= out_line_size) len = out_line_size - 1;
	      memcpy (out_line, history[history_pos], len);
	      out_line[len] = '\0';
	      *out_point = len;
	      *out_mark = 0;
	      return 1;
	    }
	}
      return 0;
    }

  /* Detect Enter (CR) — save to history. */
  if (keyseq_len == 1 && keyseq[0] == '\r')
    {
      if (line_len > 0 && history_count < MAX_HISTORY)
	{
	  history[history_count] = strndup (line, line_len);
	  history_count++;
	}
      history_pos = -1;
      return 0;
    }

  /* Any other key — reset history navigation. */
  if (history_pos >= 0)
    history_pos = -1;

  /* Demo: uppercase the entire line. */
  if (line_len > 0 && line_len < out_line_size)
    {
      for (i = 0; i < line_len; i++)
	out_line[i] = toupper ((unsigned char)line[i]);
      out_line[line_len] = '\0';
      *out_point = point;
      *out_mark = mark;
      return 1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/* Completion generation                                              */
/* ------------------------------------------------------------------ */

/* Custom completions for words starting with "@". */
char **
readline_filter_completions (const char *line, int line_len, int point,
			     const char *word, int word_start, int word_end)
{
  static const char *users[] = {
    "@alice", "@bob", "@charlie", "@dave", "@eve", NULL
  };
  const char *prefix;
  int prefix_len, count, i;
  char **matches;
  char *lcd;

  if (word[0] != '@')
    return NULL;	/* Fall through to default bash completion. */

  prefix = word;
  prefix_len = strlen (prefix);

  /* Count matching users. */
  count = 0;
  for (i = 0; users[i]; i++)
    if (strncmp (users[i], prefix, prefix_len) == 0)
      count++;

  if (count == 0)
    return NULL;

  /* Build matches array: [LCD, match1, ..., matchN, NULL]. */
  matches = malloc ((count + 2) * sizeof (char *));
  if (!matches)
    return NULL;

  /* Fill matches (skip LCD slot for now). */
  {
    int idx = 1;
    for (i = 0; users[i]; i++)
      if (strncmp (users[i], prefix, prefix_len) == 0)
	matches[idx++] = strdup (users[i]);
    matches[idx] = NULL;
  }

  /* Compute LCD (longest common prefix of all matches). */
  if (count == 1)
    {
      lcd = strdup (matches[1]);
    }
  else
    {
      lcd = strdup (matches[1]);
      for (i = 2; matches[i]; i++)
	{
	  int j;
	  for (j = 0; lcd[j] && matches[i][j] && lcd[j] == matches[i][j]; j++)
	    ;
	  lcd[j] = '\0';
	}
    }
  matches[0] = lcd;

  return matches;
}

/* ------------------------------------------------------------------ */
/* Completion display                                                 */
/* ------------------------------------------------------------------ */

void
readline_filter_display_matches (char **matches, int num_matches, int max_len)
{
  int i;
  fprintf (stderr, "\n");
  for (i = 1; i <= num_matches; i++)
    fprintf (stderr, "  * %s\n", matches[i]);
}
