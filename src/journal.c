#include "journal.h"
#include "nash_limits.h"
#include "cJSON.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/file.h> /* flock */
#include <unistd.h>   /* fdatasync, fileno */

/* Recursively unwrap nested JSON in a thought string.
 * The LLM sometimes echoes its own previous response as a thought,
 * producing double- or triple-nested JSON like:
 *   {"thought":"{\"thought\":\"{\"thought\":\"deep\"}\"}"}
 * This function keeps unwrapping while the result is still JSON with a "thought" key.
 * Returns a heap-allocated clean thought, or NULL if no unwrapping was needed.
 * Caller must free() the result. */
char *unwrap_thought(const char *thought) {
  if (!thought || thought[0] != '{') return NULL; /* nothing to unwrap */

  char *current = xstrdup(thought);

  for (int depth = 0; depth < 5; depth++) {
    cJSON *nested = cJSON_Parse(current);
    if (!nested) break;
    const char *inner = json_str(nested, "thought");
    if (!inner) {
      /* No thought field or not a string — the input is a JSON object
             * (e.g. a full action echo) with no extractable thought. */
      cJSON_Delete(nested);
      free(current);
      return NULL;
    }
    if (inner[0] == '\0') {
      /* Nested thought is empty — the model echoed the full action
             * JSON as the thought field but left thought="".  Return NULL
             * so the display layer treats this as "no thought". */
      cJSON_Delete(nested);
      free(current);
      return NULL;
    }
    char *next = xstrdup(inner);
    cJSON_Delete(nested);
    free(current);
    if (!next) return NULL;          /* OOM */
    if (next[0] != '{') return next; /* fully unwrapped — plain text */
    current = next;                  /* still JSON, continue unwrapping */
  }

  /* If we exhausted depth limit, the value is still JSON.
     * Return NULL since it's not meaningful thought text. */
  free(current);
  return NULL;
}

journal_t *journal_new(const char *session_dir) {
  journal_t *j = xcalloc(1, sizeof(*j));
  pthread_mutex_init(&j->mtx, NULL); /* FIX CRIT2: thread-safe journal */
  j->session_dir = xstrdup(session_dir);
  char path[NASH_PATH_MAX];
  snprintf(path, sizeof(path), "%s/journal.jsonl", session_dir);
  j->path = xstrdup(path);
  j->lazy_created = 1;
  return j;
}

/* Lazy journal: session directory is not created until first journal_append().
 * If program exits without any append, no session directory exists. */
journal_t *journal_new_lazy(const char *nash_dir, const char *workspace) {
  journal_t *j = xcalloc(1, sizeof(*j));
  pthread_mutex_init(&j->mtx, NULL); /* FIX CRIT2: thread-safe journal */
  j->nash_dir = xstrdup(nash_dir);
  j->workspace = (workspace && workspace[0]) ? xstrdup(workspace) : NULL;
  j->lazy_created = 0;
  return j;
}

void journal_free(journal_t *j) {
  if (!j) return;
  pthread_mutex_destroy(&j->mtx); /* FIX CRIT2 */
  free(j->path);
  free(j->session_dir);
  free(j->nash_dir);
  free(j->workspace);
  free(j);
}

const char *journal_session_dir(journal_t *j) {
  if (!j) return NULL;
  return j->session_dir;
}

/* Create the session directory lazily. Called from journal_append on first write. */
static int journal_create_lazy_session(journal_t *j) {
  if (!j || !j->nash_dir || j->lazy_created) return 0;

  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);

  char *base = sessions_base_dir(j->nash_dir, j->workspace);

  char path[1088];
  snprintf(path, sizeof(path), "%s/%ld.%05ld",
           base, (long)tp.tv_sec, tp.tv_nsec / 10000);
  free(base);
  if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;

  j->session_dir = xstrdup(path);
  char jpath[NASH_PATH_MAX];
  snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", path);
  j->path = xstrdup(jpath);
  j->lazy_created = 1;
  return 0;
}

int journal_append(journal_t *j, int react_loop, int step, const char *tool,
                   cJSON *params, const char *ref,
                   size_t size, int lines, const char *error,
                   const char *tool_call_id, double start_ts) {
  /* FIX CRIT2: mutex protects all journal state (lazy_created, path, file I/O)
     * against concurrent calls from inference thread and nash_log(). */
  pthread_mutex_lock(&j->mtx);

  /* Lazy session creation: create directory on first write */
  if (j->nash_dir && !j->lazy_created) {
    journal_create_lazy_session(j);
  }
  if (!j->path) {
    pthread_mutex_unlock(&j->mtx);
    return -1;
  }
  FILE *f = fopen(j->path, "a");
  if (!f) {
    pthread_mutex_unlock(&j->mtx);
    return -1;
  }

  /* Exclusive lock for writes — prevents torn reads from TUI thread */
  flock(fileno(f), LOCK_EX);

  /* Unix epoch timestamp with microsecond precision.
     * When start_ts > 0, use the pre-captured tool start time instead of
     * current time so the journal reflects when the tool began executing. */
  char ts[32];
  if (start_ts > 0) {
    long sec = (long)start_ts;
    long frac = (long)((start_ts - (double)sec) * 100000);
    snprintf(ts, sizeof(ts), "%ld.%05ld", sec, frac);
  } else {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    snprintf(ts, sizeof(ts), "%ld.%05ld", (long)tp.tv_sec, tp.tv_nsec / 10000);
  }

  cJSON *entry = cJSON_CreateObject();
  cJSON_AddNumberToObject(entry, "react_loop", react_loop);
  cJSON_AddNumberToObject(entry, "step", step);
  cJSON_AddStringToObject(entry, "ts", ts);
  cJSON_AddStringToObject(entry, "tool", tool);
  if (params) cJSON_AddItemToObject(entry, "params", cJSON_Duplicate(params, 1));
  if (ref) cJSON_AddStringToObject(entry, "ref", ref);
  cJSON_AddNumberToObject(entry, "size", (double)size);
  cJSON_AddNumberToObject(entry, "lines", lines);
  cJSON_AddBoolToObject(entry, "failed", error != NULL);
  if (error) cJSON_AddStringToObject(entry, "error", error);
  if (tool_call_id) cJSON_AddStringToObject(entry, "tc_id", tool_call_id);

  char *json = cJSON_PrintUnformatted(entry);
  fprintf(f, "%s\n", json);
  free(json);
  cJSON_Delete(entry);
  fflush(f);
  fdatasync(fileno(f));
  fclose(f);
  pthread_mutex_unlock(&j->mtx); /* FIX CRIT2 */
  return 0;
}

/* ── B1 FIX: Shared compaction parameter extraction ──── */

void journal_parse_compaction_stats(cJSON *params, journal_compaction_stats_t *s) {
  s->before_msgs = 0;
  s->after_msgs = 0;
  s->before_pct = 0;
  s->after_pct = 0;
  if (!params) return;
/* S3 FIX: Macro-driven extraction eliminates 4× repeated pattern. */
#define PARSE_INT_FIELD(name) \
  do { \
    s->name = json_int(params, #name, s->name); \
  } while (0)
  PARSE_INT_FIELD(before_msgs);
  PARSE_INT_FIELD(after_msgs);
  PARSE_INT_FIELD(before_pct);
  PARSE_INT_FIELD(after_pct);
#undef PARSE_INT_FIELD
}

/* ── B3 FIX: Shared structural tool skip list ────────── */

int journal_is_structural_tool(const char *tool) {
  if (!tool) return 0;
  return strcmp(tool, "system") == 0 ||
         strcmp(tool, "query") == 0 ||
         strcmp(tool, "context") == 0 ||
         strcmp(tool, "spec") == 0 ||
         strcmp(tool, "memory_context") == 0 ||
         strcmp(tool, "log") == 0 ||
         strcmp(tool, "compaction") == 0;
}

/* Reduced structural filter for lexical search.
 * Keeps "query" and "memory_context" searchable — these contain
 * user input text and matched skills/lessons that are the most
 * useful content for pattern-based search. */
int journal_is_structural_tool_search(const char *tool) {
  if (!tool) return 0;
  return strcmp(tool, "system") == 0 ||
         strcmp(tool, "context") == 0 ||
         strcmp(tool, "spec") == 0 ||
         strcmp(tool, "log") == 0 ||
         strcmp(tool, "compaction") == 0;
}

/* DUP1 FIX: Extract shared evicted-count flush logic.
 * Appends an eviction summary line to `out` and resets counters. */
static void journal_flush_evicted(str_t *out, int *evicted_count,
                                  int *evicted_compact_step,
                                  const journal_compaction_stats_t *cs) {
  if (*evicted_count <= 0) return;
  if (*evicted_compact_step >= 0)
    str_appendf(out,
                "    ... [%d steps compacted at step %d: %d\xe2\x86\x92%d msgs, %d%%\xe2\x86\x92%d%% usage]\n",
                *evicted_count, *evicted_compact_step,
                cs->before_msgs, cs->after_msgs,
                cs->before_pct, cs->after_pct);
  else
    str_appendf(out, "    ... [%d earlier steps evicted from context]\n",
                *evicted_count);
  *evicted_count = 0;
  *evicted_compact_step = -1;
}

char *journal_manifest(journal_t *j, int max_steps) {
  /* Delegate to the filtered version with no eviction filtering.
     * target_loop=-1 ensures the eviction check never matches. */
  return journal_manifest_filtered(j, max_steps, -1, 0);
}

/* FIX D8: Build a manifest that collapses evicted steps into a summary.
 * Steps in the target react_loop with step < min_step are shown as a
 * single "[N earlier steps evicted from context]" line. This prevents
 * the model from trying to reference detailed step info that was evicted. */
char *journal_manifest_filtered(journal_t *j, int max_steps,
                                int target_loop, int min_step) {
  /* FIX CRIT2: mutex protects j->path from concurrent lazy creation */
  pthread_mutex_lock(&j->mtx);
  if (!j->path) {
    pthread_mutex_unlock(&j->mtx);
    return xstrdup("Session history: (empty — new session)");
  }
  FILE *f = fopen(j->path, "r");
  pthread_mutex_unlock(&j->mtx); /* path is stable after lazy init */
  if (!f) return xstrdup("Session history: (empty — new session)");
  flock(fileno(f), LOCK_SH);

  str_t out = str_new(2048);
  str_append_cstr(&out, "Session history:\n");

  char line[NASH_LINE_MAX];
  int count = 0;
  int current_loop = -1;
  int evicted_count = 0;                       /* count of evicted steps in target_loop */
  int evicted_compact_step = -1;               /* step of last compaction in evicted range */
  journal_compaction_stats_t evicted_cs = {0}; /* B1 FIX */

  while (fgets(line, sizeof(line), f) && count < max_steps) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;

    int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
    int step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));
    const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
    const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
    double sz = cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));
    cJSON *params = cJSON_GetObjectItem(entry, "params");

    /* New react loop — show header with query text */
    if (loop != current_loop) {
      /* B2 FIX: Flush evicted count from previous loop */
      journal_flush_evicted(&out, &evicted_count,
                            &evicted_compact_step, &evicted_cs);
      current_loop = loop;
      if (loop > 0) str_append_cstr(&out, "\n");
      str_appendf(&out, "  [Query R%d]", loop);

      if (tool && strcmp(tool, "query") == 0 && params) {
        const char *qtext = json_str(params, "text");
        if (qtext) {
          char truncated[101];
          utf8_truncate(truncated, qtext, 100);
          str_appendf(&out, " \"%s\"", truncated);
        }
      }
      str_append_cstr(&out, "\n");
    }

    /* FIX D8: For steps in the target loop that are below min_step,
         * just count them — they were evicted from context */
    if (loop == target_loop && step > 0 && step < min_step) {
      /* B1 FIX: Track compaction events within evicted range */
      if (tool && strcmp(tool, "compaction") == 0 && params) {
        evicted_compact_step = step;
        journal_parse_compaction_stats(params, &evicted_cs);
      }
      evicted_count++;
      cJSON_Delete(entry);
      count++;
      continue;
    }

    /* B1 FIX: Render compaction entries as a compact separator line */
    if (tool && strcmp(tool, "compaction") == 0 && params) {
      journal_compaction_stats_t cs;
      journal_parse_compaction_stats(params, &cs);
      str_appendf(&out,
                  "    \xe2\x9c\x82 context compacted at step %d: %d\xe2\x86\x92%d msgs, %d%%\xe2\x86\x92%d%%\n",
                  step, cs.before_msgs, cs.after_msgs, cs.before_pct, cs.after_pct);
      cJSON_Delete(entry);
      count++;
      continue;
    }

    /* B3 FIX: Skip structural entries (shown in header / redundant) */
    if (journal_is_structural_tool(tool)) {
      cJSON_Delete(entry);
      count++;
      continue;
    }

    /* Extract thought and key param for display */
    const char *key_param = "";
    const char *thought = NULL;
    char *unwrapped2 = NULL;
    if (params) {
      const char *th = json_str(params, "thought");
      if (th && th[0]) {
        unwrapped2 = unwrap_thought(th);
        if (unwrapped2)
          thought = unwrapped2;
        else if (th[0] != '{')
          thought = th; /* plain text, use as-is */
                        /* else: JSON with no extractable thought — skip */
      }
      const char *cmd_s = json_str(params, "command");
      const char *p_s = json_str(params, "path");
      const char *pat_s = json_str(params, "pattern");
      const char *res_s = json_str(params, "result");
      if (cmd_s)
        key_param = cmd_s;
      else if (p_s)
        key_param = p_s;
      else if (pat_s)
        key_param = pat_s;
      else if (res_s)
        key_param = res_s;
    }

    /* Show thought above the step line (truncated for manifest) */
    if (thought) {
      char th_trunc[121];
      utf8_truncate(th_trunc, thought, 120);
      str_appendf(&out, "  \xf0\x9f\x92\xad %s\n", th_trunc);
    }

    char buf[512];
    cJSON *failed_j = cJSON_GetObjectItem(entry, "failed");
    int failed = (failed_j && cJSON_IsTrue(failed_j));
    const char *mark = failed ? "x" : "+";

    char kp[101];
    utf8_truncate(kp, key_param, 80);

    if (tool && strcmp(tool, "done") == 0) {
      char kp_done[101];
      utf8_truncate(kp_done, key_param, 100);
      snprintf(buf, sizeof(buf), "    %s %s: -> \"%s\"",
               mark, ref ? ref : "?", kp_done);
    } else if (failed) {
      snprintf(buf, sizeof(buf), "    %s %s: %s \"%s\"",
               mark, ref ? ref : "?", tool ? tool : "?", kp);
    } else {
      snprintf(buf, sizeof(buf), "    %s %s: %s \"%s\" -> %d chars",
               mark, ref ? ref : "?", tool ? tool : "?", kp, (int)sz);
    }
    str_append_cstr(&out, buf);
    str_append_cstr(&out, "\n");

    free(unwrapped2);
    unwrapped2 = NULL;
    cJSON_Delete(entry);
    count++;
  }

  /* Flush final evicted count */
  journal_flush_evicted(&out, &evicted_count,
                        &evicted_compact_step, &evicted_cs);

  fclose(f);
  return str_steal(&out);
}

/* ── Chunk extraction for session-level RAG ─────────── */

void journal_chunks_free(journal_chunks_t *jc) {
  if (!jc) return;
  for (int i = 0; i < jc->n_chunks; i++)
    free(jc->texts[i]);
  free(jc->texts);
  jc->texts = NULL;
  jc->n_chunks = 0;
}

/* Read first N bytes of a file, return heap string (NULL on error) */
static char *read_file_head(const char *path, int max_bytes) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  char *buf = xmalloc((size_t)max_bytes + 1);
  size_t n = fread(buf, 1, (size_t)max_bytes, f);
  fclose(f);
  buf[n] = '\0';
  /* Clamp to valid UTF-8 boundary */
  while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80)
    n--;
  buf[n] = '\0';
  return buf;
}

journal_chunks_t journal_extract_chunks(const char *session_dir,
                                        int max_chars_per_chunk,
                                        int max_chunks) {
  journal_chunks_t result = {0};
  if (!session_dir) return result;
  if (max_chars_per_chunk <= 0) max_chars_per_chunk = 900;
  if (max_chunks <= 0) max_chunks = 50;

  char jpath[NASH_PATH_MAX];
  snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);

  FILE *f = fopen(jpath, "r");
  if (!f) return result;
  flock(fileno(f), LOCK_SH);

  /* Pass 1: extract per-step semantic text segments */
  typedef struct {
    char *text;
  } seg_t;
  int seg_count = 0, seg_cap = 64;
  seg_t *segs = xcalloc((size_t)seg_cap, sizeof(seg_t));

  /* Track query text for chunk prefixes */
  char query_text[201] = {0};

  char line[NASH_LINE_MAX];
  while (fgets(line, sizeof(line), f)) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;

    const char *tool = cJSON_GetStringValue(
      cJSON_GetObjectItem(entry, "tool"));
    cJSON *params = cJSON_GetObjectItem(entry, "params");
    const char *ref = cJSON_GetStringValue(
      cJSON_GetObjectItem(entry, "ref"));

    if (!tool) {
      cJSON_Delete(entry);
      continue;
    }

    /* Capture query text for chunk context prefix */
    if (strcmp(tool, "query") == 0 && params && !query_text[0]) {
      const char *qt = json_str(params, "text");
      if (qt) {
        utf8_truncate(query_text, qt, 200);
      }
      cJSON_Delete(entry);
      continue;
    }

    /* B3 FIX: Skip structural entries — no semantic value for RAG */
    if (journal_is_structural_tool(tool)) {
      cJSON_Delete(entry);
      continue;
    }

    str_t seg = str_new(512);

    /* Extract thought — highest semantic value */
    if (params) {
      const char *th_s = json_str(params, "thought");
      if (th_s && th_s[0]) {
        char *unwrapped = unwrap_thought(th_s);
        const char *thought = unwrapped ? unwrapped
                                        : (th_s[0] != '{' ? th_s : NULL);
        if (thought && strlen(thought) > 5) {
          char trunc[401];
          utf8_truncate(trunc, thought, 400);
          str_appendf(&seg, "%s\n", trunc);
        }
        free(unwrapped);
      }
    }

    /* Tool-specific content extraction */
    if (strcmp(tool, "done") == 0 && params) {
      const char *res_s = json_str(params, "result");
      if (res_s && strlen(res_s) > 5) {
        char trunc[501];
        utf8_truncate(trunc, res_s, 500);
        str_appendf(&seg, "Result: %s\n", trunc);
      }
    } else if (strcmp(tool, "memory_store") == 0 && params) {
      const char *key_s = json_str(params, "key");
      const char *val_s = json_str(params, "value");
      if (key_s)
        str_appendf(&seg, "Stored: %s\n", key_s);
      if (val_s) {
        char trunc[301];
        utf8_truncate(trunc, val_s, 300);
        str_appendf(&seg, "%s\n", trunc);
      }
    } else if (strcmp(tool, "memory_search") == 0 && params) {
      const char *q_s = json_str(params, "query");
      if (q_s)
        str_appendf(&seg, "Searched: %s\n", q_s);
    } else if ((strcmp(tool, "file_read") == 0 ||
                strcmp(tool, "grep_search") == 0 ||
                strcmp(tool, "shell_exec") == 0 ||
                strcmp(tool, "file_edit") == 0 ||
                strcmp(tool, "web_fetch") == 0) &&
               ref) {
      /* Read first N chars of referenced content */
      char ref_path[NASH_PATH_MAX];
      path_join(ref_path, sizeof(ref_path), session_dir, ref);
      int content_limit = (strcmp(tool, "shell_exec") == 0) ? 300 : 400;
      char *content = read_file_head(ref_path, content_limit);
      if (content && strlen(content) > 10) {
        /* Add tool context */
        if (params) {
          const char *p_s = json_str(params, "path");
          const char *cmd_s = json_str(params, "command");
          const char *pat_s = json_str(params, "pattern");
          if (p_s)
            str_appendf(&seg, "%s %s: ", tool, p_s);
          else if (cmd_s) {
            char ct[101];
            utf8_truncate(ct, cmd_s, 100);
            str_appendf(&seg, "shell: %s\n", ct);
          } else if (pat_s)
            str_appendf(&seg, "%s '%s': ", tool, pat_s);
          else
            str_appendf(&seg, "%s: ", tool);
        }
        str_appendf(&seg, "%s\n", content);
      }
      free(content);
    }

    /* Only keep segments with meaningful content */
    if (seg.len > 15) {
      if (seg_count >= seg_cap) {
        seg_cap *= 2;
        if (safe_realloc((void **)&segs, (size_t)seg_cap * sizeof(seg_t))) {
          str_free(&seg);
          break;
        }
      }
      segs[seg_count].text = str_steal(&seg);
      seg_count++;
    } else {
      str_free(&seg);
    }

    cJSON_Delete(entry);
  }
  fclose(f);

  if (seg_count == 0) {
    free(segs);
    return result;
  }

  /* Pass 2: Group segments into chunks of ~max_chars_per_chunk */
  /* Build a session context prefix */
  char prefix[256];
  {
    const char *base = strrchr(session_dir, '/');
    if (base)
      base++;
    else
      base = session_dir;
    double ts = atof(base);
    time_t ts_t = (time_t)ts;
    char date_buf[32];
    format_iso_date(ts_t, date_buf, sizeof(date_buf));
    if (query_text[0])
      snprintf(prefix, sizeof(prefix), "Session %s | Query: %s\n",
               date_buf, query_text);
    else
      snprintf(prefix, sizeof(prefix), "Session %s\n", date_buf);
  }
  int prefix_len = (int)strlen(prefix);
  int content_budget = max_chars_per_chunk - prefix_len;
  if (content_budget < 200) content_budget = 200;

  /* Allocate chunks array (upper bound = seg_count) */
  int chunk_cap = seg_count < max_chunks ? seg_count : max_chunks;
  result.texts = xcalloc((size_t)chunk_cap + 1, sizeof(char *));
  if (!result.texts) {
    for (int i = 0; i < seg_count; i++)
      free(segs[i].text);
    free(segs);
    return result;
  }

  str_t cur_chunk = str_new((size_t)max_chars_per_chunk + 128);
  str_append_cstr(&cur_chunk, prefix);
  int chunk_content_len = 0;

  for (int i = 0; i < seg_count; i++) {
    int seg_len = (int)strlen(segs[i].text);

    /* Would adding this segment exceed budget? Start new chunk. */
    if (chunk_content_len > 0 &&
        chunk_content_len + seg_len > content_budget) {
      /* Save current chunk */
      if (result.n_chunks < chunk_cap) {
        result.texts[result.n_chunks] = str_steal(&cur_chunk);
        result.n_chunks++;
      } else {
        str_free(&cur_chunk);
      }
      /* Start new chunk with prefix */
      cur_chunk = str_new((size_t)max_chars_per_chunk + 128);
      str_append_cstr(&cur_chunk, prefix);
      chunk_content_len = 0;

      if (result.n_chunks >= max_chunks) {
        /* Hit chunk cap — discard remaining segments */
        for (int j = i; j < seg_count; j++)
          free(segs[j].text);
        break;
      }
    }

    /* Append segment to current chunk */
    if (seg_len > content_budget) {
      /* Single oversized segment: truncate (UTF-8 safe) */
      size_t safe_len = utf8_clamp(segs[i].text, (size_t)content_budget);
      str_append(&cur_chunk, segs[i].text, safe_len);
      chunk_content_len += content_budget;
    } else {
      str_append_cstr(&cur_chunk, segs[i].text);
      chunk_content_len += seg_len;
    }
    free(segs[i].text);
    segs[i].text = NULL;
  }

  /* Save final chunk if it has content */
  if (chunk_content_len > 0 && result.n_chunks < max_chunks) {
    result.texts[result.n_chunks] = str_steal(&cur_chunk);
    result.n_chunks++;
  } else {
    str_free(&cur_chunk);
  }

  free(segs);
  return result;
}

/* Scan journal.jsonl and return the highest react_loop value found.
 * Returns -1 if the journal is empty or doesn't exist. */
int journal_max_react_loop(journal_t *j) {
  if (!j) return -1;

  pthread_mutex_lock(&j->mtx);
  if (!j->path) {
    pthread_mutex_unlock(&j->mtx);
    return -1;
  }
  FILE *f = fopen(j->path, "r");
  pthread_mutex_unlock(&j->mtx); /* path is stable after read */
  if (!f) return -1;
  flock(fileno(f), LOCK_SH); /* shared lock for reading */

  int max_loop = -1;
  char line[NASH_LINE_MAX];
  while (fgets(line, sizeof(line), f)) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;
    int loop = (int)cJSON_GetNumberValue(
      cJSON_GetObjectItem(entry, "react_loop"));
    if (loop > max_loop) max_loop = loop;
    cJSON_Delete(entry);
  }
  fclose(f);
  return max_loop;
}

/* ── SIGIL-inspired mandatory tool verification (arxiv 2607.27309) ──
 *
 * Post-pass gate that verifies mechanical steps were actually executed.
 * Addresses the "call narrated, not made" failure mode: the model
 * describes a tool invocation in its reasoning but never issues the
 * tool_call, so the step appears complete while the work was skipped.
 *
 * Returns the count of missing tools (0 = pass). */
int journal_check_required_tools(journal_t *j, int react_loop,
                                 char **required, int n_required,
                                 char **missing) {
  if (missing) *missing = NULL;
  if (!j || n_required <= 0 || !required) return 0;

  /* Bit vector: seen[i] = 1 when required[i] found in journal */
  int *seen = calloc((size_t)n_required, sizeof(int));
  if (!seen) return n_required;

  pthread_mutex_lock(&j->mtx);
  if (!j->path) {
    pthread_mutex_unlock(&j->mtx);
    free(seen);
    return n_required;
  }
  FILE *f = fopen(j->path, "r");
  pthread_mutex_unlock(&j->mtx);
  if (!f) {
    free(seen);
    return n_required;
  }
  flock(fileno(f), LOCK_SH);

  char line[NASH_LINE_MAX];
  while (fgets(line, sizeof(line), f)) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;

    /* Only check entries from the target react_loop */
    cJSON *rl = cJSON_GetObjectItem(entry, "react_loop");
    if (!rl || (int)cJSON_GetNumberValue(rl) != react_loop) {
      cJSON_Delete(entry);
      continue;
    }

    const char *tool = json_str(entry, "tool");
    if (tool) {
      for (int i = 0; i < n_required; i++) {
        if (!seen[i] && strcmp(tool, required[i]) == 0)
          seen[i] = 1;
      }
    }
    cJSON_Delete(entry);
  }
  fclose(f);

  /* Count and format missing tools */
  int n_missing = 0;
  for (int i = 0; i < n_required; i++) {
    if (!seen[i]) n_missing++;
  }

  if (n_missing > 0 && missing) {
    /* Build comma-separated list of missing tool names */
    size_t len = 0;
    for (int i = 0; i < n_required; i++) {
      if (!seen[i])
        len += strlen(required[i]) + 2; /* ", " */
    }
    char *buf = malloc(len + 1);
    if (buf) {
      buf[0] = '\0';
      int first = 1;
      for (int i = 0; i < n_required; i++) {
        if (!seen[i]) {
          if (!first) strcat(buf, ", ");
          strcat(buf, required[i]);
          first = 0;
        }
      }
      *missing = buf;
    }
  }

  free(seen);
  return n_missing;
}
