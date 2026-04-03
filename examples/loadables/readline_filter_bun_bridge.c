/*
 * readline_filter_bun_bridge.c -- Bridge between bash readline and a Bun server.
 *
 * This shared library implements the READLINE_INPUT_FILTER_LIB API and
 * forwards all events to a Bun (or any other) server via a Unix domain
 * socket.  The protocol is newline-delimited JSON.
 *
 * The socket path is taken from the READLINE_FILTER_SOCKET environment
 * variable, defaulting to "/tmp/readline-filter.sock".
 *
 * Build:
 *   gcc -shared -fPIC -o readline_filter_bun_bridge.so readline_filter_bun_bridge.c
 *
 * Usage:
 *   # Start the Bun server first (see examples/readline-filter-bun/)
 *   bun run server.ts &
 *
 *   # Then load the bridge
 *   export READLINE_INPUT_FILTER_LIB=./readline_filter_bun_bridge.so
 *   bash
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#define DEFAULT_SOCKET_PATH "/tmp/readline-filter.sock"
#define BUF_SIZE 16384
#define CONNECT_TIMEOUT_MS 100
#define IO_TIMEOUT_MS 5

static int sock_fd = -1;
static char sock_path[256];

/* ------------------------------------------------------------------ */
/* Socket connection management                                       */
/* ------------------------------------------------------------------ */

static int
try_connect (void)
{
  struct sockaddr_un addr;
  int fd, flags;

  fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, sock_path, sizeof (addr.sun_path) - 1);

  /* Set non-blocking for connect timeout. */
  flags = fcntl (fd, F_GETFL, 0);
  fcntl (fd, F_SETFL, flags | O_NONBLOCK);

  if (connect (fd, (struct sockaddr *)&addr, sizeof (addr)) < 0)
    {
      if (errno == EINPROGRESS || errno == EAGAIN)
	{
	  fd_set wfds;
	  struct timeval tv;
	  FD_ZERO (&wfds);
	  FD_SET (fd, &wfds);
	  tv.tv_sec = 0;
	  tv.tv_usec = CONNECT_TIMEOUT_MS * 1000;
	  if (select (fd + 1, NULL, &wfds, NULL, &tv) <= 0)
	    {
	      close (fd);
	      return -1;
	    }
	  /* Check if connect succeeded. */
	  {
	    int err = 0;
	    socklen_t len = sizeof (err);
	    getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &len);
	    if (err != 0)
	      {
		close (fd);
		return -1;
	      }
	  }
	}
      else
	{
	  close (fd);
	  return -1;
	}
    }

  /* Restore blocking mode for I/O. */
  fcntl (fd, F_SETFL, flags);

  return fd;
}

static void
ensure_connected (void)
{
  if (sock_fd >= 0)
    return;
  sock_fd = try_connect ();
}

static void
disconnect (void)
{
  if (sock_fd >= 0)
    {
      close (sock_fd);
      sock_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* JSON helpers (minimal, no dependencies)                            */
/* ------------------------------------------------------------------ */

/* Escape a string for JSON.  OUT must have room for at least len*6+3 bytes. */
static int
json_escape (const char *src, int src_len, char *out, int out_size)
{
  int i, pos = 0;

  if (pos >= out_size) return -1;
  out[pos++] = '"';

  for (i = 0; i < src_len && pos < out_size - 2; i++)
    {
      unsigned char c = (unsigned char)src[i];
      if (c == '"')
	{ if (pos + 2 > out_size - 1) break; out[pos++] = '\\'; out[pos++] = '"'; }
      else if (c == '\\')
	{ if (pos + 2 > out_size - 1) break; out[pos++] = '\\'; out[pos++] = '\\'; }
      else if (c == '\n')
	{ if (pos + 2 > out_size - 1) break; out[pos++] = '\\'; out[pos++] = 'n'; }
      else if (c == '\r')
	{ if (pos + 2 > out_size - 1) break; out[pos++] = '\\'; out[pos++] = 'r'; }
      else if (c == '\t')
	{ if (pos + 2 > out_size - 1) break; out[pos++] = '\\'; out[pos++] = 't'; }
      else if (c < 0x20)
	{
	  if (pos + 6 > out_size - 1) break;
	  pos += snprintf (out + pos, out_size - pos, "\\u%04x", c);
	}
      else
	out[pos++] = c;
    }

  out[pos++] = '"';
  out[pos] = '\0';
  return pos;
}

/* Build a JSON array of ints from raw bytes (for keyseq). */
static int
json_byte_array (const char *src, int src_len, char *out, int out_size)
{
  int i, pos = 0;

  out[pos++] = '[';
  for (i = 0; i < src_len && pos < out_size - 5; i++)
    {
      if (i > 0)
	out[pos++] = ',';
      pos += snprintf (out + pos, out_size - pos, "%d", (unsigned char)src[i]);
    }
  out[pos++] = ']';
  out[pos] = '\0';
  return pos;
}

/* ------------------------------------------------------------------ */
/* Send request, receive response (newline-delimited JSON)            */
/* ------------------------------------------------------------------ */

/* Send a JSON message and read a single-line JSON response.
   Returns the number of bytes in response, or -1 on error.
   Response is null-terminated in resp_buf. */
static int
send_recv (const char *request, int req_len, char *resp_buf, int resp_size)
{
  ssize_t nw, nr;
  fd_set rfds;
  struct timeval tv;
  int pos;
  char *env_timeout;
  long timeout_ms;

  ensure_connected ();
  if (sock_fd < 0)
    return -1;

  /* Write the full request. */
  nw = write (sock_fd, request, req_len);
  if (nw < 0)
    {
      disconnect ();
      return -1;
    }

  /* Get timeout from READLINE_INPUT_FILTER_TIMEOUT or use IO default. */
  env_timeout = getenv ("READLINE_INPUT_FILTER_TIMEOUT");
  if (env_timeout && *env_timeout)
    {
      timeout_ms = strtol (env_timeout, NULL, 10);
      if (timeout_ms == 0)
	timeout_ms = -1;	/* infinite */
    }
  else
    timeout_ms = IO_TIMEOUT_MS;

  /* Wait for response with timeout. */
  FD_ZERO (&rfds);
  FD_SET (sock_fd, &rfds);
  if (timeout_ms > 0)
    {
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
    }

  if (select (sock_fd + 1, &rfds, NULL, NULL,
	      timeout_ms > 0 ? &tv : NULL) <= 0)
    return -1;		/* timeout or error — fallback to default behavior */

  /* Read until newline. */
  pos = 0;
  while (pos < resp_size - 1)
    {
      nr = read (sock_fd, &resp_buf[pos], 1);
      if (nr == 1)
	{
	  if (resp_buf[pos] == '\n')
	    break;
	  pos++;
	}
      else if (nr == 0)
	{
	  /* EOF — server closed connection. */
	  disconnect ();
	  return -1;
	}
      else
	{
	  if (errno == EAGAIN || errno == EWOULDBLOCK)
	    break;
	  disconnect ();
	  return -1;
	}
    }

  resp_buf[pos] = '\0';
  return pos;
}

/* ------------------------------------------------------------------ */
/* Simple JSON response parsing                                       */
/* ------------------------------------------------------------------ */

/* Find a string value for a key in a flat JSON object.
   Returns pointer into json (not null-terminated), sets *len.
   Returns NULL if not found. */
static const char *
json_find_string (const char *json, const char *key, int *len)
{
  char search[128];
  const char *p, *start;
  int escape;

  snprintf (search, sizeof (search), "\"%s\"", key);
  p = strstr (json, search);
  if (!p)
    return NULL;

  p += strlen (search);
  while (*p == ' ' || *p == ':')
    p++;

  if (*p != '"')
    return NULL;

  p++;	/* skip opening quote */
  start = p;
  escape = 0;
  while (*p)
    {
      if (escape)
	{ escape = 0; p++; continue; }
      if (*p == '\\')
	{ escape = 1; p++; continue; }
      if (*p == '"')
	break;
      p++;
    }

  *len = p - start;
  return start;
}

/* Find an integer value for a key.  Returns -1 if not found. */
static int
json_find_int (const char *json, const char *key)
{
  char search[128];
  const char *p;

  snprintf (search, sizeof (search), "\"%s\"", key);
  p = strstr (json, search);
  if (!p)
    return -1;

  p += strlen (search);
  while (*p == ' ' || *p == ':')
    p++;

  return (int)strtol (p, NULL, 10);
}

/* Find a boolean value for a key.  Returns 0 if not found or false. */
static int
json_find_bool (const char *json, const char *key)
{
  char search[128];
  const char *p;

  snprintf (search, sizeof (search), "\"%s\"", key);
  p = strstr (json, search);
  if (!p)
    return 0;

  p += strlen (search);
  while (*p == ' ' || *p == ':')
    p++;

  return (*p == 't');	/* "true" starts with 't' */
}

/* Unescape a JSON string in-place.  Returns new length. */
static int
json_unescape (const char *src, int src_len, char *dst, int dst_size)
{
  int i, pos = 0;

  for (i = 0; i < src_len && pos < dst_size - 1; i++)
    {
      if (src[i] == '\\' && i + 1 < src_len)
	{
	  i++;
	  switch (src[i])
	    {
	    case '"':  dst[pos++] = '"'; break;
	    case '\\': dst[pos++] = '\\'; break;
	    case 'n':  dst[pos++] = '\n'; break;
	    case 'r':  dst[pos++] = '\r'; break;
	    case 't':  dst[pos++] = '\t'; break;
	    default:   dst[pos++] = src[i]; break;
	    }
	}
      else
	dst[pos++] = src[i];
    }

  dst[pos] = '\0';
  return pos;
}

/* Parse a JSON array of strings.  Returns malloc'd NULL-terminated array
   of malloc'd strings (matches[0]=LCD, matches[1..N]=candidates).
   Returns NULL if "matches" is null in the JSON. */
static char **
json_parse_matches (const char *json)
{
  const char *p;
  char search[] = "\"matches\"";
  char **result;
  int count, capacity, in_string, escape;
  const char *str_start;

  p = strstr (json, search);
  if (!p)
    return NULL;

  p += strlen (search);
  while (*p == ' ' || *p == ':')
    p++;

  if (*p == 'n')	/* null */
    return NULL;
  if (*p != '[')
    return NULL;

  p++;	/* skip '[' */

  /* Count and extract strings. */
  capacity = 16;
  result = malloc (capacity * sizeof (char *));
  count = 0;

  while (*p)
    {
      while (*p == ' ' || *p == ',')
	p++;
      if (*p == ']')
	break;
      if (*p != '"')
	break;

      p++;	/* skip opening quote */
      str_start = p;
      escape = 0;
      while (*p)
	{
	  if (escape)
	    { escape = 0; p++; continue; }
	  if (*p == '\\')
	    { escape = 1; p++; continue; }
	  if (*p == '"')
	    break;
	  p++;
	}

      /* Extract this string. */
      {
	int slen = p - str_start;
	char *s = malloc (slen + 1);
	json_unescape (str_start, slen, s, slen + 1);

	if (count + 2 >= capacity)
	  {
	    capacity *= 2;
	    result = realloc (result, capacity * sizeof (char *));
	  }
	result[count++] = s;
      }

      if (*p == '"')
	p++;	/* skip closing quote */
    }

  result[count] = NULL;
  return (count > 0) ? result : (free (result), (char **)NULL);
}

/* ------------------------------------------------------------------ */
/* READLINE_INPUT_FILTER_LIB API implementation                       */
/* ------------------------------------------------------------------ */

int
readline_filter_init (void)
{
  const char *env_path;

  env_path = getenv ("READLINE_FILTER_SOCKET");
  if (env_path && *env_path)
    strncpy (sock_path, env_path, sizeof (sock_path) - 1);
  else
    strncpy (sock_path, DEFAULT_SOCKET_PATH, sizeof (sock_path) - 1);
  sock_path[sizeof (sock_path) - 1] = '\0';

  sock_fd = try_connect ();
  if (sock_fd < 0)
    {
      fprintf (stderr, "bash: readline_filter_bun_bridge: cannot connect to %s\n",
	       sock_path);
      return -1;
    }

  return 0;
}

void
readline_filter_cleanup (void)
{
  disconnect ();
}

int
readline_filter_keypress (const char *keyseq, int keyseq_len,
			  const char *line, int line_len,
			  int point, int mark,
			  char *out_line, int out_line_size,
			  int *out_point, int *out_mark)
{
  char req[BUF_SIZE], resp[BUF_SIZE];
  char escaped_line[BUF_SIZE];
  char keyseq_arr[256];
  int req_len, resp_len;
  const char *new_line;
  int new_line_len, new_point, new_mark;

  json_escape (line, line_len, escaped_line, sizeof (escaped_line));
  json_byte_array (keyseq, keyseq_len, keyseq_arr, sizeof (keyseq_arr));

  req_len = snprintf (req, sizeof (req),
    "{\"type\":\"keypress\",\"keyseq\":%s,\"line\":%s,\"point\":%d,\"mark\":%d}\n",
    keyseq_arr, escaped_line, point, mark);

  resp_len = send_recv (req, req_len, resp, sizeof (resp));
  if (resp_len <= 0)
    return 0;

  if (!json_find_bool (resp, "modified"))
    return 0;

  new_line = json_find_string (resp, "line", &new_line_len);
  if (!new_line)
    return 0;

  json_unescape (new_line, new_line_len, out_line, out_line_size);

  new_point = json_find_int (resp, "point");
  new_mark = json_find_int (resp, "mark");
  *out_point = (new_point >= 0) ? new_point : point;
  *out_mark = (new_mark >= 0) ? new_mark : mark;

  return 1;
}

char **
readline_filter_completions (const char *line, int line_len, int point,
			     const char *word, int word_start, int word_end)
{
  char req[BUF_SIZE], resp[BUF_SIZE];
  char escaped_line[BUF_SIZE], escaped_word[BUF_SIZE];
  int req_len, resp_len;

  json_escape (line, line_len, escaped_line, sizeof (escaped_line));
  json_escape (word, strlen (word), escaped_word, sizeof (escaped_word));

  req_len = snprintf (req, sizeof (req),
    "{\"type\":\"complete\",\"line\":%s,\"point\":%d,"
    "\"word\":%s,\"wordStart\":%d,\"wordEnd\":%d}\n",
    escaped_line, point, escaped_word, word_start, word_end);

  resp_len = send_recv (req, req_len, resp, sizeof (resp));
  if (resp_len <= 0)
    return NULL;

  return json_parse_matches (resp);
}

void
readline_filter_display_matches (char **matches, int num_matches, int max_len)
{
  char req[BUF_SIZE], resp[BUF_SIZE];
  int req_len, pos, i;

  pos = snprintf (req, sizeof (req),
    "{\"type\":\"display\",\"matches\":[");

  for (i = 0; i <= num_matches && pos < (int)sizeof (req) - 100; i++)
    {
      char escaped[1024];
      if (i > 0)
	req[pos++] = ',';
      json_escape (matches[i], strlen (matches[i]), escaped, sizeof (escaped));
      pos += snprintf (req + pos, sizeof (req) - pos, "%s", escaped);
    }

  pos += snprintf (req + pos, sizeof (req) - pos,
    "],\"numMatches\":%d,\"maxLen\":%d}\n", num_matches, max_len);

  req_len = pos;
  send_recv (req, req_len, resp, sizeof (resp));
}
