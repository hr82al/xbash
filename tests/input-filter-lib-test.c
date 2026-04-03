/* input-filter-lib-test.c -- Test shared library for READLINE_INPUT_FILTER_LIB.
   Uppercases the line on every keypress. Provides completions for "@" prefix. */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int init_called = 0;
static int cleanup_called = 0;

int
readline_filter_init (void)
{
  init_called = 1;
  return 0;
}

void
readline_filter_cleanup (void)
{
  cleanup_called = 1;
}

int
readline_filter_keypress (const char *keyseq, int keyseq_len,
			  const char *line, int line_len,
			  int point, int mark,
			  char *out_line, int out_line_size,
			  int *out_point, int *out_mark)
{
  int i;

  if (line_len <= 0 || line_len >= out_line_size)
    return 0;

  for (i = 0; i < line_len; i++)
    out_line[i] = toupper ((unsigned char)line[i]);
  out_line[line_len] = '\0';

  *out_point = point;
  *out_mark = mark;
  return 1;
}

char **
readline_filter_completions (const char *line, int line_len, int point,
			     const char *word, int word_start, int word_end)
{
  static const char *users[] = { "@alice", "@bob", NULL };
  char **matches;
  int count = 0, i;

  if (word[0] != '@')
    return NULL;

  for (i = 0; users[i]; i++)
    if (strncmp (users[i], word, strlen (word)) == 0)
      count++;

  if (count == 0)
    return NULL;

  matches = malloc ((count + 2) * sizeof (char *));
  matches[0] = strdup ("@");	/* LCD */
  {
    int idx = 1;
    for (i = 0; users[i]; i++)
      if (strncmp (users[i], word, strlen (word)) == 0)
	matches[idx++] = strdup (users[i]);
    matches[idx] = NULL;
  }

  return matches;
}
