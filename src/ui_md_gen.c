/*
 * ui_md_gen.c — Markdown generation for session.md and reactRX.md
 *
 * Extracted from ui_state.c.  Contains the two largest functions:
 *   - ui_state_generate_session_md()  (session overview with query tree)
 *   - ui_state_generate_react_md()    (per-react-loop step log)
 * Plus supporting helpers for text extraction and file I/O.
 */

#include "ui_state_internal.h"
#include "scratchpad.h"

#include "tools.h"

/* ── Local helpers ───────────────────────────────────────── */

/* Sanitize text for use inside MD link [text](uri) syntax. */
static const char *sanitize_md_link(const char *text) {
  static char buf[512];
  int j = 0;
  if (!text) return "";
  for (int i = 0; text[i] && j < (int)sizeof(buf) - 1; i++) {
    switch (text[i]) {
      case ']':
        buf[j++] = ')';
        break;
      case '[':
        buf[j++] = '(';
        break;
      case '\n':
        buf[j++] = ' ';
        break;
      case '\r':
        break;
      default:
        buf[j++] = text[i];
        break;
    }
  }
  buf[j] = '\0';
  return buf;
}

/* Read last N lines from a file. Returns malloc'd string or NULL. */
static char *read_last_lines(const char *path, int n_lines) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;

  /* Read entire file (cap at 64KB for preview) */
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return xstrdup("");
  }
  if (sz > NASH_LINE_MAX) {
    fseek(f, sz - NASH_LINE_MAX, SEEK_SET);
    sz = NASH_LINE_MAX;
  } else {
    fseek(f, 0, SEEK_SET);
  }
  char *buf = xmalloc((size_t)sz + 1);
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = '\0';

  /* Find the start of the last n_lines */
  int count = 0;
  char *p = buf + rd;
  /* Skip trailing newline */
  if (p > buf && *(p - 1) == '\n') p--;
  while (p > buf && count < n_lines) {
    p--;
    if (*p == '\n') count++;
  }
  if (*p == '\n') p++;

  char *result = xstrdup(p);
  free(buf);
  return result;
}


/* Write string to file atomically, delegates to write_file() (str.h). */
static void write_md_file(const char *path, const char *content) {
  if (content)
    write_file(path, content, strlen(content));
}

/* Extract tool description from journal params (for step display). */
static const char *extract_desc(const char *tool, cJSON *params) {
  if (!params) return "";
  const char *path_s = json_str(params, "path");
  const char *pat_s = json_str(params, "pattern");
  const char *res_s = json_str(params, "result");
  const char *url_s = json_str(params, "url");

  if (strcmp(tool, "grep_search") == 0 && pat_s) {
    static char grep_desc[256];
    if (path_s && path_s[0])
      snprintf(grep_desc, sizeof(grep_desc), "%s %s", pat_s, path_s);
    else
      snprintf(grep_desc, sizeof(grep_desc), "%s", pat_s);
    return grep_desc;
  }
  if (strcmp(tool, "file_read") == 0 && path_s) {
    int s = json_int(params, "start_line", 0);
    int e = json_int(params, "end_line", 0);
    if (s || e) {
      static char fr_desc[256];
      if (s > 0 && e > 0)
        snprintf(fr_desc, sizeof(fr_desc), "%s:%d-%d", path_s, s, e);
      else if (s > 0)
        snprintf(fr_desc, sizeof(fr_desc), "%s:%d-EOF", path_s, s);
      else
        snprintf(fr_desc, sizeof(fr_desc), "%s", path_s);
      return fr_desc;
    }
    return path_s;
  }
  if ((strcmp(tool, "file_edit") == 0 || strcmp(tool, "file_write") == 0) && path_s)
    return path_s;
  /* image_analyze: show just the filename (no path=) + question */
  if (strcmp(tool, "image_analyze") == 0 && path_s) {
    static char ia_desc[256];
    const char *base = strrchr(path_s, '/');
    base = base ? base + 1 : path_s;
    const char *q = json_str(params, "question");
    if (q && q[0]) {
      int qlen = (int)strlen(q);
      int trunc = (qlen > 80);
      if (trunc) qlen = 80;
      snprintf(ia_desc, sizeof(ia_desc), "%s %.*s%s",
               base, qlen, q, trunc ? "..." : "");
    } else {
      snprintf(ia_desc, sizeof(ia_desc), "%s", base);
    }
    return ia_desc;
  }
  if ((strcmp(tool, "web_fetch") == 0 || strcmp(tool, "web_search") == 0) && url_s)
    return url_s;
  if (strcmp(tool, "shell_exec") == 0)
    return json_str_or(params, "command", "");
  /* Context injection sections: ctx:memory, ctx:temporal, etc.
     * Show human-readable size and section-specific labels. */
  if (strncmp(tool, "ctx:", 4) == 0) {
    static char ctx_desc[128];
    int size = json_int(params, "size", 0);
    const char *section = tool + 4;
    if (size > 0) {
      const char *unit = "chars";
      int display = size;
      if (size >= 1000) {
        display = size / 1000;
        unit = "K";
      }
      snprintf(ctx_desc, sizeof(ctx_desc), "%s (%d%s)", section, display, unit);
    } else {
      snprintf(ctx_desc, sizeof(ctx_desc), "%s", section);
    }
    return ctx_desc;
  }
  /* System prompt: show model name */
  if (strcmp(tool, "system") == 0) {
    return json_str_or(params, "model", "system prompt");
  }
  /* Legacy context entry (pre-split) */
  if (strcmp(tool, "context") == 0) {
    int nm = json_int(params, "n_messages", -1);
    if (nm >= 0) {
      static char ctx_desc_legacy[64];
      snprintf(ctx_desc_legacy, sizeof(ctx_desc_legacy), "%d messages", nm);
      return ctx_desc_legacy;
    }
    return "full LLM context";
  }
  /* User query: show truncated first line */
  if (strcmp(tool, "query") == 0) {
    const char *text_s = json_str(params, "text");
    if (text_s) {
      static char query_desc[128];
      const char *s = text_s;
      const char *nl = strchr(s, '\n');
      int len = nl ? (int)(nl - s) : (int)strlen(s);
      if (len > 80) len = 80;
      snprintf(query_desc, sizeof(query_desc), "%.*s%s",
               len, s, (nl || (int)strlen(s) > 80) ? "..." : "");
      return query_desc;
    }
    return "user query";
  }
  /* User redirect (ad-hoc prompt typed during inference) */
  if (strcmp(tool, "redirect") == 0) {
    const char *text_s = json_str(params, "text");
    if (text_s) {
      static char redir_desc[128];
      const char *s = text_s;
      const char *nl = strchr(s, '\n');
      int len = nl ? (int)(nl - s) : (int)strlen(s);
      if (len > 80) len = 80;
      snprintf(redir_desc, sizeof(redir_desc), "%.*s%s",
               len, s, (nl || (int)strlen(s) > 80) ? "..." : "");
      return redir_desc;
    }
    return "user redirect";
  }
  /* Memory recall context: show matched count summary */
  if (strcmp(tool, "memory_context") == 0) {
    static char mc_desc[128];
    cJSON *skills = cJSON_GetObjectItem(params, "skills_matched");
    cJSON *lessons = cJSON_GetObjectItem(params, "lessons_matched");
    cJSON *strategies = cJSON_GetObjectItem(params, "strategies_matched");
    int ns = skills ? cJSON_GetArraySize(skills) : 0;
    int nl = lessons ? cJSON_GetArraySize(lessons) : 0;
    int nst = strategies ? cJSON_GetArraySize(strategies) : 0;
    int total = ns + nl + nst;
    if (total > 0)
      snprintf(mc_desc, sizeof(mc_desc), "recall: %d matched (%ds %dl %dst)",
               total, ns, nl, nst);
    else
      snprintf(mc_desc, sizeof(mc_desc), "recall context");
    return mc_desc;
  }
  /* System log entries: show the message */
  if (strcmp(tool, "log") == 0) {
    return json_str_or(params, "message", "(system log)");
  }
  /* Server error entries: show the error message */
  if (strcmp(tool, "server_error") == 0) {
    const char *err_s = json_str(params, "error");
    if (err_s) return err_s;
    return json_str_or(params, "server_message", "LLM server error");
  }
  /* Prediction summary: show totals */
  if (strcmp(tool, "prediction_summary") == 0) {
    static char psum_desc[128];
    int total = 0, confirmed = 0, refuted = 0;
    cJSON *child = params->child;
    while (child) {
      total += json_int(child, "total", 0);
      confirmed += json_int(child, "confirmed", 0);
      refuted += json_int(child, "refuted", 0);
      child = child->next;
    }
    if (total > 0)
      snprintf(psum_desc, sizeof(psum_desc),
               "%d predictions: %d confirmed, %d refuted",
               total, confirmed, refuted);
    else
      snprintf(psum_desc, sizeof(psum_desc), "no predictions");
    return psum_desc;
  }
  /* Plan: add_item shows a compact inline description;
   * other plan ops show nothing inline (full plan rendered below). */
  if (strcmp(tool, "plan") == 0) {
    const char *op = json_str(params, "op");
    if (op && strcmp(op, "add_item") == 0) {
      static char plan_desc[256];
      const char *text_s = json_str(params, "text");
      if (text_s && text_s[0]) {
        int tlen = (int)strlen(text_s);
        int trunc = (tlen > 120);
        if (trunc) tlen = 120;
        snprintf(plan_desc, sizeof(plan_desc), "%.*s%s",
                 tlen, text_s, trunc ? "..." : "");
        return plan_desc;
      }
    }
    return "";
  }
  /* Truncate done result to first line, max 80 chars */
  if (strcmp(tool, "done") == 0 && res_s) {
    static char trunc_desc[128];
    const char *s = res_s;
    /* Find first newline */
    const char *nl = strchr(s, '\n');
    int len = nl ? (int)(nl - s) : (int)strlen(s);
    if (len > 80) len = 80;
    snprintf(trunc_desc, sizeof(trunc_desc), "%.*s%s",
             len, s, (nl || (int)strlen(s) > 80) ? "..." : "");
    return trunc_desc;
  }
  /* device_control: show "subcommand param=val ..." instead of
     * "command=subcommand param=val ..." — the "command" param acts as
     * a subcommand and reads better without the key= prefix. */
  if (strcmp(tool, "device_control") == 0) {
    static char dc_desc[512];
    int pos = 0;
    const char *cmd_s = json_str(params, "command");
    if (cmd_s) {
      int clen = (int)strlen(cmd_s);
      if (clen > (int)sizeof(dc_desc) - 2) clen = (int)sizeof(dc_desc) - 2;
      memcpy(dc_desc, cmd_s, (size_t)clen);
      pos = clen;
    }
    cJSON *child = params->child;
    while (child && pos < (int)sizeof(dc_desc) - 2) {
      if (!child->string ||
          strcmp(child->string, "thought") == 0 ||
          strcmp(child->string, "action") == 0 ||
          strcmp(child->string, "command") == 0) {
        child = child->next;
        continue;
      }
      if (cJSON_IsBool(child) && !cJSON_IsTrue(child)) {
        child = child->next;
        continue;
      }
      if (pos > 0) dc_desc[pos++] = ' ';
      /* Always show key= for remaining params */
      int klen = (int)strlen(child->string);
      int room = (int)sizeof(dc_desc) - 1 - pos;
      if (room < klen + 2) break;
      memcpy(dc_desc + pos, child->string, (size_t)klen);
      pos += klen;
      dc_desc[pos++] = '=';
      if (child->valuestring) {
        int vlen = (int)strlen(child->valuestring);
        int trunc = (vlen > 60);
        if (trunc) vlen = 60;
        room = (int)sizeof(dc_desc) - 4 - pos;
        if (vlen > room) {
          vlen = room;
          trunc = 1;
        }
        if (vlen > 0) {
          memcpy(dc_desc + pos, child->valuestring, (size_t)vlen);
          pos += vlen;
        }
        if (trunc && pos < (int)sizeof(dc_desc) - 4) {
          memcpy(dc_desc + pos, "...", 3);
          pos += 3;
        }
      } else if (cJSON_IsNumber(child)) {
        room = (int)sizeof(dc_desc) - 1 - pos;
        int n = snprintf(dc_desc + pos, (size_t)room, "%g",
                         cJSON_GetNumberValue(child));
        if (n > 0 && n < room) pos += n;
      } else if (cJSON_IsBool(child)) {
        room = (int)sizeof(dc_desc) - 1 - pos;
        if (room >= 4) {
          memcpy(dc_desc + pos, "true", 4);
          pos += 4;
        }
      }
      child = child->next;
    }
    dc_desc[pos] = '\0';
    if (pos > 0) return dc_desc;
    return "";
  }
  /* Generic fallback: show params compactly (skip "thought").
     * - Single visible param: show just value, no key= prefix
     * - Multiple visible params: show key=value pairs
     * - Skip default-valued params (false booleans)
     * Truncate individual values at 60 chars. */
  {
    static char generic_desc[512];
    int pos = 0;

    /* First pass: count visible (non-thought, non-action, non-default) params */
    int n_visible = 0;
    cJSON *child = params->child;
    while (child) {
      if (child->string &&
          strcmp(child->string, "thought") != 0 &&
          strcmp(child->string, "action") != 0) {
        /* Skip false booleans (default values) */
        if (!(cJSON_IsBool(child) && !cJSON_IsTrue(child)))
          n_visible++;
      }
      child = child->next;
    }
    int single_param = (n_visible == 1);

    child = params->child;
    while (child && pos < (int)sizeof(generic_desc) - 2) {
      if (!child->string) {
        child = child->next;
        continue;
      }
      /* Skip thought — rendered separately */
      if (strcmp(child->string, "thought") == 0) {
        child = child->next;
        continue;
      }
      /* Skip action — redundant with tool name column */
      if (strcmp(child->string, "action") == 0) {
        child = child->next;
        continue;
      }
      /* Skip false booleans (default values) */
      if (cJSON_IsBool(child) && !cJSON_IsTrue(child)) {
        child = child->next;
        continue;
      }
      /* Add separator */
      if (pos > 0)
        generic_desc[pos++] = ' ';
      /* key= prefix (omit for single-param tools) */
      if (!single_param) {
        int klen = (int)strlen(child->string);
        int room = (int)sizeof(generic_desc) - 1 - pos;
        if (room < klen + 2) break;
        memcpy(generic_desc + pos, child->string, (size_t)klen);
        pos += klen;
        generic_desc[pos++] = '=';
      }
      if (child->valuestring) {
        int vlen = (int)strlen(child->valuestring);
        int trunc = (vlen > 60);
        if (trunc) vlen = 60;
        int room = (int)sizeof(generic_desc) - 4 - pos;
        if (vlen > room) {
          vlen = room;
          trunc = 1;
        }
        if (vlen > 0) {
          memcpy(generic_desc + pos, child->valuestring, (size_t)vlen);
          pos += vlen;
        }
        if (trunc && pos < (int)sizeof(generic_desc) - 4) {
          memcpy(generic_desc + pos, "...", 3);
          pos += 3;
        }
      } else if (cJSON_IsNumber(child)) {
        int room = (int)sizeof(generic_desc) - 1 - pos;
        int n = snprintf(generic_desc + pos, (size_t)room, "%g",
                         cJSON_GetNumberValue(child));
        if (n > 0 && n < room) pos += n;
      } else if (cJSON_IsBool(child)) {
        /* Only true booleans reach here (false already skipped) */
        int room = (int)sizeof(generic_desc) - 1 - pos;
        if (room >= 4) {
          memcpy(generic_desc + pos, "true", 4);
          pos += 4;
        }
      }
      child = child->next;
    }
    generic_desc[pos] = '\0';
    if (pos > 0) return generic_desc;
  }
  return "";
}

/* Extract thought from params, unwrapping nested JSON if needed.
 * Delegates to the shared unwrap_thought() in journal.c. */
static char *extract_thought(cJSON *params) {
  if (!params) return NULL;
  const char *th = json_str(params, "thought");
  if (!th || !th[0]) return NULL;

  /* Skip whitespace-only thoughts (e.g. "\n\n" emitted before tool calls) */
  if (is_whitespace_only(th)) return NULL;

  char *clean = unwrap_thought(th);
  if (clean) return clean;
  /* unwrap_thought returned NULL — if the raw value is JSON (starts with
     * '{'), it's a garbled echo with no extractable thought; suppress it.
     * Otherwise it's plain text — use as-is. */
  if (th[0] == '{') return NULL;
  return xstrdup(th);
}

/* ── Session MD generation ───────────────────────────────── */

void ui_state_generate_session_md(ui_state_t *ui) {
  if (!ui) return;

  /* Always generate session.md in the main session directory so the
     * user can see all playbook passes listed with navigation links.
     * Previously, agent_view redirected to playbook_session_dir which
     * placed session.md in whatever pass dir was active, making it
     * invisible from the main session view. */
  const char *session_dir = ui->session_dir;
  if (!session_dir) return;

  str_t md = str_new(8192);

  /* Banner */
  if (ui->banner && ui->banner[0]) {
    str_append_cstr(&md, ui->banner);
    str_append_cstr(&md, "\n---\n\n");
  }

  /* Read journal for query list */
  char jpath[NASH_PATH_MAX];
  snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);
  FILE *f = fopen(jpath, "r");
  if (!f) {
    /* No main journal — if playbook passes exist, skip to reading
         * their journals below.  Otherwise show empty state. */
    if (ui->pb_pass_count == 0) {
      if (md.len == 0)
        str_append_cstr(&md, "# Nash\n\n*No session loaded*\n");
      goto write_out;
    }
  }

  /* Collect query info (with tree structure support) */
  typedef struct {
    char *text;
    double ts;
    int react_loop;
    int parent_loop; /* -1 = root, else parent react_loop ID */
    int step_count;
    char *result;
    int done;
    char *session_dir; /* NULL = main session, else playbook pass dir */
    char *pass_label;  /* NULL = normal query, else playbook pass label */
  } qinfo_t;

  qinfo_t *qinfos = NULL;
  int qcount = 0, qcap = 0;

  char line[NASH_LINE_MAX];
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      cJSON *entry = cJSON_Parse(line);
      if (!entry) continue;

      const char *tool = json_str(entry, "tool");
      int loop = json_int(entry, "react_loop", 0);

      if (tool && strcmp(tool, "query") == 0) {
        /* Check if a placeholder was already created by memory_context */
        qinfo_t *qi = NULL;
        for (int i = 0; i < qcount; i++) {
          if (qinfos[i].react_loop == loop) {
            qi = &qinfos[i];
            break;
          }
        }
        if (!qi) {
          if (qcount >= qcap) {
            int new_cap = qcap ? qcap * 2 : 16;
            if (safe_realloc((void **)&qinfos, (size_t)new_cap * sizeof(qinfo_t))) {
              cJSON_Delete(entry);
              continue;
            }
            qcap = new_cap;
          }
          qi = &qinfos[qcount++];
          memset(qi, 0, sizeof(*qi));
        }
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        free(qi->text); /* free any placeholder text from memory_context */
        qi->text = xstrdup(json_str_or(params, "text", "?"));
        const char *ts_s = json_str(entry, "ts");
        qi->ts = ts_s ? atof(ts_s) : 0;
        qi->react_loop = loop;
        /* Parse parent_loop from journal (backward compat: default -1 = root) */
        qi->parent_loop = json_int(params, "parent_loop", -1);
      } else if (tool && strcmp(tool, "memory_context") == 0) {
        /* Fallback: if a react loop was interrupted after memory_context
             * was logged but before the "query" entry was written, use the
             * memory_context's "query" field to make the loop visible. */
        int already_known = 0;
        for (int i = 0; i < qcount; i++) {
          if (qinfos[i].react_loop == loop) {
            already_known = 1;
            break;
          }
        }
        if (!already_known) {
          if (qcount >= qcap) {
            int new_cap = qcap ? qcap * 2 : 16;
            if (safe_realloc((void **)&qinfos, (size_t)new_cap * sizeof(qinfo_t))) {
              cJSON_Delete(entry);
              continue;
            }
            qcap = new_cap;
          }
          qinfo_t *qi = &qinfos[qcount++];
          memset(qi, 0, sizeof(*qi));
          cJSON *params = cJSON_GetObjectItem(entry, "params");
          qi->text = xstrdup(json_str_or(params, "query", "(interrupted)"));
          const char *ts_s = json_str(entry, "ts");
          qi->ts = ts_s ? atof(ts_s) : 0;
          qi->react_loop = loop;
          qi->parent_loop = -1; /* unknown parent — treat as root */
        }
      } else if (tool && strcmp(tool, "query") != 0 && strcmp(tool, "system") != 0 && strncmp(tool, "ctx:", 4) != 0) {
        for (int i = qcount - 1; i >= 0; i--) {
          if (qinfos[i].react_loop == loop) {
            qinfos[i].step_count++;
            if (strcmp(tool, "done") == 0) {
              qinfos[i].done = 1;
              cJSON *params = cJSON_GetObjectItem(entry, "params");
              const char *res_done = json_str(params, "result");
              if (res_done)
                str_replace(&qinfos[i].result, res_done);
            }
            break;
          }
        }
      }
      cJSON_Delete(entry);
    }
    fclose(f);
  } /* if (f) */

  /* ── Read playbook pass journals ──────────────────────── */
  for (int pi = 0; pi < ui->pb_pass_count; pi++) {
    pb_pass_info_t *pbi = &ui->pb_passes[pi];
    if (!pbi->session_dir) continue;

    char pjpath[NASH_PATH_MAX];
    snprintf(pjpath, sizeof(pjpath), "%s/journal.jsonl", pbi->session_dir);
    FILE *pf = fopen(pjpath, "r");
    if (!pf) continue;

    while (fgets(line, sizeof(line), pf)) {
      cJSON *entry = cJSON_Parse(line);
      if (!entry) continue;

      const char *tool = json_str(entry, "tool");
      int loop = json_int(entry, "react_loop", 0);

      if (tool && strcmp(tool, "query") == 0) {
        /* Check if already known (shared session mode: same journal) */
        qinfo_t *qi = NULL;
        for (int i = 0; i < qcount; i++) {
          if (qinfos[i].react_loop == loop &&
              qinfos[i].session_dir &&
              strcmp(qinfos[i].session_dir, pbi->session_dir) == 0) {
            qi = &qinfos[i];
            break;
          }
        }
        if (!qi) {
          if (qcount >= qcap) {
            int new_cap = qcap ? qcap * 2 : 16;
            if (safe_realloc((void **)&qinfos, (size_t)new_cap * sizeof(qinfo_t))) {
              cJSON_Delete(entry);
              continue;
            }
            qcap = new_cap;
          }
          qi = &qinfos[qcount++];
          memset(qi, 0, sizeof(*qi));
        }
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        free(qi->text);
        qi->text = xstrdup(json_str_or(params, "text", "?"));
        const char *ts_s = json_str(entry, "ts");
        qi->ts = ts_s ? atof(ts_s) : 0;
        qi->react_loop = loop;
        qi->parent_loop = -1; /* playbook passes are always roots */
        str_replace(&qi->session_dir, pbi->session_dir);
        free(qi->pass_label);
        qi->pass_label = pbi->pass_label ? xstrdup(pbi->pass_label) : NULL;
      } else if (tool && strcmp(tool, "query") != 0 && strcmp(tool, "system") != 0 && strncmp(tool, "ctx:", 4) != 0) {
        /* Count steps and detect done — match by session_dir + loop */
        for (int i = qcount - 1; i >= 0; i--) {
          if (qinfos[i].react_loop == loop &&
              qinfos[i].session_dir &&
              strcmp(qinfos[i].session_dir, pbi->session_dir) == 0) {
            qinfos[i].step_count++;
            if (strcmp(tool, "done") == 0) {
              qinfos[i].done = 1;
              cJSON *params = cJSON_GetObjectItem(entry, "params");
              const char *res_done = json_str(params, "result");
              if (res_done)
                str_replace(&qinfos[i].result, res_done);
            }
            break;
          }
        }
      }
      cJSON_Delete(entry);
    }
    fclose(pf);
  }

  str_append_cstr(&md, "## Session History\n\n");

  /* ── Tree-order rendering via DFS ── */
  {
    /* Build render order via iterative DFS.
         * Nodes without a matching parent in qinfos are treated as roots.
         * Uses visited[] to prevent cycles from causing infinite loops. */
    size_t qalloc = qcount > 0 ? (size_t)qcount : 1;
    int *render_order = xmalloc(qalloc * sizeof(int));
    int *render_depth = xmalloc(qalloc * sizeof(int));
    int *visited = xcalloc(qalloc, sizeof(int));
    int rcount = 0;

    /* Stack sized 2*qcount to handle branching safely */
    int stack_cap = qcount > 0 ? qcount * 2 : 1;
    int *dfs_stack = xmalloc((size_t)stack_cap * sizeof(int));
    int *dfs_depth = xmalloc((size_t)stack_cap * sizeof(int));
    if (!render_order || !render_depth || !visited || !dfs_stack || !dfs_depth) {
      free(render_order);
      free(render_depth);
      free(visited);
      free(dfs_stack);
      free(dfs_depth);
      for (int i = 0; i < qcount; i++) {
        free(qinfos[i].text);
        free(qinfos[i].result);
        free(qinfos[i].session_dir);
        free(qinfos[i].pass_label);
      }
      free(qinfos);
      goto write_out;
    }
    int stop = 0;

    /* Find roots: parent_loop == -1, self-referencing (parent == self),
         * or parent not found in qinfos.
         * Push in reverse order so first root is processed first. */
    for (int i = qcount - 1; i >= 0; i--) {
      int is_root = (qinfos[i].parent_loop < 0 ||
                     qinfos[i].parent_loop == qinfos[i].react_loop);
      if (!is_root) {
        /* Check if parent exists in qinfos */
        int found = 0;
        for (int j = 0; j < qcount; j++) {
          if (qinfos[j].react_loop == qinfos[i].parent_loop) {
            found = 1;
            break;
          }
        }
        if (!found) is_root = 1; /* orphan → treat as root */
      }
      if (is_root && stop < stack_cap) {
        dfs_stack[stop] = i;
        dfs_depth[stop] = 0;
        stop++;
      }
    }

    while (stop > 0) {
      stop--;
      int idx = dfs_stack[stop];
      int depth = dfs_depth[stop];

      if (visited[idx]) continue; /* cycle guard */
      visited[idx] = 1;

      render_order[rcount] = idx;
      render_depth[rcount] = depth;
      rcount++;

      /* Push children (reverse order for correct DFS traversal) */
      for (int i = qcount - 1; i >= 0; i--) {
        if (!visited[i] && i != idx &&
            qinfos[i].parent_loop == qinfos[idx].react_loop &&
            stop < stack_cap) {
          dfs_stack[stop] = i;
          dfs_depth[stop] = depth + 1;
          stop++;
        }
      }
    }

    /* Render in DFS order with tree indentation */
    for (int ri = 0; ri < rcount; ri++) {
      int i = render_order[ri];
      int depth = render_depth[ri];
      qinfo_t *qi = &qinfos[i];

      /* Format timestamp */
      char ts_buf[32] = "";
      if (qi->ts > 0) {
        format_iso_datetime((time_t)qi->ts, ts_buf, sizeof(ts_buf));
      }

      /* Effective session dir for this query */
      const char *qi_dir = qi->session_dir ? qi->session_dir : ui->session_dir;

      /* Status icon */
      int is_active = (ui->status == STATUS_RUNNING &&
                       qi->react_loop == ui->current_react_loop &&
                       (!qi->session_dir || (ui->playbook_session_dir &&
                                             strcmp(qi->session_dir, ui->playbook_session_dir) == 0)));
      const char *icon = is_active ? "⟳" : (qi->done ? "✓" : "▶");

      /* Tree indentation (2 spaces per depth level) */
      for (int d = 0; d < depth; d++)
        str_append_cstr(&md, "  ");

      /* Build display text: include pass label for playbook entries.
             * For playbook passes, show just the label (the full prompt
             * template is too verbose for the session overview). */
      char display_text[512];
      if (qi->pass_label) {
        snprintf(display_text, sizeof(display_text), "%s",
                 qi->pass_label);
      } else {
        snprintf(display_text, sizeof(display_text), "%s",
                 sanitize_md_link(qi->text));
      }

      /* Query as hyperlink to reactRX.md.
             * For playbook passes in different dirs, use absolute path. */
      if (qi->session_dir) {
        str_appendf(&md, "[%s %s  %s](%s/reactR%d.md)\n",
                    icon, ts_buf, display_text,
                    qi->session_dir, qi->react_loop);
      } else {
        str_appendf(&md, "[%s %s  %s](reactR%d.md)\n",
                    icon, ts_buf, display_text, qi->react_loop);
      }

      /* Preview: show for the ACTIVE react loop, or if user toggled
             * with 'c' key (URI in expanded_uris). */
      char react_uri[NASH_PATH_MAX];
      if (qi->session_dir)
        snprintf(react_uri, sizeof(react_uri), "%s/reactR%d.md",
                 qi->session_dir, qi->react_loop);
      else
        snprintf(react_uri, sizeof(react_uri), "reactR%d.md", qi->react_loop);
      int is_expanded = 0;
      for (int ei = 0; ei < ui->expanded_count; ei++) {
        if (strcmp(ui->expanded_uris[ei], react_uri) == 0) {
          is_expanded = 1;
          break;
        }
      }
      if (is_active || is_expanded) {
        char rpath[NASH_PATH_MAX];
        snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
                 qi_dir, qi->react_loop);
        char *preview = read_last_lines(rpath, 10);
        if (preview && preview[0]) {
          str_append_cstr(&md, preview);
          /* Ensure trailing newline */
          if (preview[strlen(preview) - 1] != '\n')
            str_append_cstr(&md, "\n");
        }
        free(preview);
      }
    }

    free(render_order);
    free(render_depth);
    free(visited);
    free(dfs_stack);
    free(dfs_depth);
  }

  for (int i = 0; i < qcount; i++) {
    free(qinfos[i].text);
    free(qinfos[i].result);
    free(qinfos[i].session_dir);
    free(qinfos[i].pass_label);
  }
  free(qinfos);

write_out:;
  char *md_str = str_steal(&md);
  char spath[NASH_PATH_MAX];
  snprintf(spath, sizeof(spath), "%s/session.md", session_dir);
  write_md_file(spath, md_str);
  free(md_str);
}

/* ── React MD generation ─────────────────────────────────── */

void ui_state_generate_react_md(ui_state_t *ui, int react_loop) {
  if (!ui || !ui->session_dir) return;

  /* Use playbook session dir if active, else main session dir */
  const char *eff_dir = ui->playbook_session_dir
                          ? ui->playbook_session_dir
                          : ui->session_dir;

  str_t md = str_new(8192);

  /* Read journal for this react loop's entries */
  char jpath[NASH_PATH_MAX];
  snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", eff_dir);
  FILE *f = fopen(jpath, "r");
  if (!f) {
    str_free(&md);
    return;
  }

  /* Collect all entries for this react loop into an array */
  typedef struct {
    char *tool;
    char *desc;
    char *thought;
    char *ref;
    char *compact_desc; /* non-NULL = compaction separator, not a normal step */
    int step;
    int size;
    int failed;
    double ts;
    char *child_dir; /* subtask only: basename of child session dir */
  } step_info_t;

  step_info_t *steps = NULL;
  int nsteps = 0, scap = 0;
  char *query_text = NULL;
  char line[NASH_LINE_MAX];

  /* Collect child_dir names from subtask entries across ALL react loops.
   * Used by the in-flight subtask indicator to avoid showing completed
   * subtasks from prior loops as "running..." in the current loop. */
  char **all_child_dirs = NULL;
  int n_all_child_dirs = 0, cap_all_child_dirs = 0;

  /* Local accumulators for per-loop stats reconstructed from journal.
   * These allow the stats footer to display for ANY react loop,
   * not just the currently-active one. */
  int j_prompt_tokens = 0, j_completion_tokens = 0;
  double j_gen_tps = 0, j_pp_tps = 0;
  int j_llm_steps = 0;

  while (fgets(line, sizeof(line), f)) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;

    int loop = json_int(entry, "react_loop", 0);
    const char *tool = json_str(entry, "tool");

    /* Track subtask child_dirs from ALL loops (not just current) so
     * the in-flight indicator knows which subtasks already completed. */
    if (tool && strcmp(tool, "subtask") == 0) {
      cJSON *params = cJSON_GetObjectItem(entry, "params");
      const char *cd = params ? json_str(params, "child_dir") : NULL;
      if (cd) {
        if (n_all_child_dirs >= cap_all_child_dirs) {
          cap_all_child_dirs = cap_all_child_dirs ? cap_all_child_dirs * 2 : 8;
          char **tmp = xmalloc((size_t)cap_all_child_dirs * sizeof(char *));
          if (all_child_dirs) {
            memcpy(tmp, all_child_dirs, (size_t)n_all_child_dirs * sizeof(char *));
            free(all_child_dirs);
          }
          all_child_dirs = tmp;
        }
        all_child_dirs[n_all_child_dirs++] = xstrdup(cd);
      }
    }

    if (loop != react_loop || !tool) {
      cJSON_Delete(entry);
      continue;
    }

    if (strcmp(tool, "query") == 0) {
      cJSON *params = cJSON_GetObjectItem(entry, "params");
      const char *text_s = json_str(params, "text");
      if (text_s)
        str_replace(&query_text, text_s);
      /* Fall through to step collection so query appears as a
             * browsable step with clickable store ref in reactRX.md */
    }

    if (strcmp(tool, "memory_context") == 0) {
      /* Fallback query text for interrupted loops (no "query" entry) */
      if (!query_text) {
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        const char *qtext = json_str(params, "query");
        if (qtext)
          query_text = xstrdup(qtext);
      }
      /* Fall through to step collection so recall context appears
             * as a browsable step with clickable store ref in reactRX.md */
    }

    /* Skip internal-only entries that belong in the journal audit
         * trail but not in the user-facing reactRX.md display. */
    if (strcmp(tool, "checkpoint_restore") == 0 ||
        strcmp(tool, "spec") == 0) {
      cJSON_Delete(entry);
      continue;
    }

    /* B1 FIX: Compaction entries → collect as separator (not a normal step) */
    if (strcmp(tool, "compaction") == 0) {
      cJSON *params = cJSON_GetObjectItem(entry, "params");
      journal_compaction_stats_t cs;
      journal_parse_compaction_stats(params, &cs);
      if (nsteps >= scap) {
        int new_cap = scap ? scap * 2 : 32;
        if (safe_realloc((void **)&steps, (size_t)new_cap * sizeof(step_info_t))) {
          cJSON_Delete(entry);
          continue;
        }
        scap = new_cap;
      }
      step_info_t *si = &steps[nsteps++];
      memset(si, 0, sizeof(*si));
      si->tool = xstrdup(tool);
      si->step = json_int(entry, "step", 0);
      const char *ts_s = json_str(entry, "ts");
      si->ts = ts_s ? atof(ts_s) : 0;
      char cdesc[128];
      snprintf(cdesc, sizeof(cdesc),
               "\xe2\x9c\x82 context compacted: %d\xe2\x86\x92%d msgs, %d%%\xe2\x86\x92%d%%",
               cs.before_msgs, cs.after_msgs, cs.before_pct, cs.after_pct);
      si->compact_desc = xstrdup(cdesc);
      cJSON_Delete(entry);
      continue;
    }

    /* Skip internal provider log entries — they clutter the TUI
         * with debug info (token counts, timing) that belongs in the
         * journal audit trail but not in the user-facing display.
         * Exception: error/failure messages ARE shown so the user
         * can see connection problems, auth failures, etc. */
    if (strcmp(tool, "log") == 0) {
      cJSON *params_log = cJSON_GetObjectItem(entry, "params");
      const char *m = json_str_or(params_log, "message", "");

      /* Extract per-step token stats from provider log lines so the
       * stats footer can be reconstructed for any react loop (not
       * just the currently-active one). Format:
       *   [provider/complete] final stats: prompt_tokens=N ... completion_tokens=N pp=F gen=F */
      const char *sp;
      if ((sp = strstr(m, "prompt_tokens=")) != NULL) {
        int pt = 0, ct = 0;
        double pp = 0, gen = 0;
        pt = atoi(sp + 14);
        const char *cp = strstr(m, "completion_tokens=");
        if (cp) ct = atoi(cp + 18);
        const char *gp = strstr(m, "gen=");
        if (gp) gen = atof(gp + 4);
        const char *ppp = strstr(m, "pp=");
        if (ppp) pp = atof(ppp + 3);
        j_prompt_tokens += pt;
        j_completion_tokens += ct;
        if (gen > 0) j_gen_tps = gen;   /* last value, most representative */
        if (pp > 0) j_pp_tps = pp;
        j_llm_steps++;
      }

      if (!ui_ci_strstr(m, "error") && !ui_ci_strstr(m, "failed") &&
          !ui_ci_strstr(m, "timed out")) {
        cJSON_Delete(entry);
        continue;
      }
      /* Fall through: error log entries are displayed as steps */
    }

    /* Collect step info */
    if (nsteps >= scap) {
      int new_cap = scap ? scap * 2 : 32;
      if (safe_realloc((void **)&steps, (size_t)new_cap * sizeof(step_info_t))) {
        cJSON_Delete(entry);
        continue;
      }
      scap = new_cap;
    }
    step_info_t *si = &steps[nsteps++];
    memset(si, 0, sizeof(*si));

    si->tool = xstrdup(tool);
    si->step = json_int(entry, "step", 0);
    const char *ts_s = json_str(entry, "ts");
    si->ts = ts_s ? atof(ts_s) : 0;
    const char *ref_s = json_str(entry, "ref");
    si->ref = ref_s ? xstrdup(ref_s) : NULL;
    si->size = json_int(entry, "size", 0);
    si->failed = json_bool(entry, "failed", 0);

    cJSON *params = cJSON_GetObjectItem(entry, "params");
    char *thought = extract_thought(params);
    si->thought = thought;
    si->desc = xstrdup(extract_desc(tool, params));

    /* For subtask: extract child_dir from params for reactR0.md link */
    if (strcmp(tool, "subtask") == 0 && params) {
      const char *cd = json_str(params, "child_dir");
      si->child_dir = cd ? xstrdup(cd) : NULL;
    }

    cJSON_Delete(entry);
  }
  fclose(f);

  /* Compute max tool name length for column alignment */
  int max_tool_len = 0;
  for (int i = 0; i < nsteps; i++) {
    int tl = (int)strlen(steps[i].tool);
    if (tl > max_tool_len) max_tool_len = tl;
  }

  /* Header — for multi-line queries, show first line as heading
     * and remaining lines as a blockquote block */
  if (query_text && query_text[0]) {
    const char *nl = strchr(query_text, '\n');
    if (nl) {
      /* Multi-line query: first line as heading */
      str_append_cstr(&md, "# Query: ");
      str_append(&md, query_text, (size_t)(nl - query_text));
      str_append_cstr(&md, "\n\n");
      /* Remaining lines as blockquote */
      const char *rest = nl + 1;
      while (*rest) {
        const char *eol = strchr(rest, '\n');
        str_append_cstr(&md, "> ");
        if (eol) {
          str_append(&md, rest, (size_t)(eol - rest));
          str_append_cstr(&md, "\n");
          rest = eol + 1;
        } else {
          str_append_cstr(&md, rest);
          str_append_cstr(&md, "\n");
          break;
        }
      }
      str_append_cstr(&md, "\n");
    } else {
      str_appendf(&md, "# Query: %s\n\n", query_text);
    }
  } else {
    str_appendf(&md, "# Query: ?\n\n");
  }

  /* Render each step in nashell-style compact format */
  for (int i = 0; i < nsteps; i++) {
    step_info_t *si = &steps[i];
    int is_last = (i == nsteps - 1);

    /* Compaction separator — render as visual divider, not a step */
    if (si->compact_desc) {
      str_append_cstr(&md, "---\n");
      str_append_cstr(&md, si->compact_desc);
      str_append_cstr(&md, "\n---\n");
      continue;
    }

    /* Server error — render as prominent error banner so the user
         * can immediately see the session died and why. */
    if (strcmp(si->tool, "server_error") == 0) {
      str_append_cstr(&md, "\n---\n");
      /* \xe2\x9d\x8c = ❌ */
      str_appendf(&md, "## \xe2\x9d\x8c LLM Error\n\n");
      if (si->desc && si->desc[0])
        str_appendf(&md, "%s\n", si->desc);
      else
        str_append_cstr(&md, "LLM server returned an unrecoverable error.\n");
      str_append_cstr(&md, "\n---\n");
      continue;
    }

    /* Compute elapsed time from previous step */
    double elapsed = 0;
    if (i > 0 && si->ts > 0 && steps[i - 1].ts > 0)
      elapsed = si->ts - steps[i - 1].ts;

    /* Format elapsed */
    char elapsed_str[32] = "";
    if (elapsed > 0) {
      int es = (int)elapsed;
      if (es > 0)
        snprintf(elapsed_str, sizeof(elapsed_str), " (%ds)", es);
    }

    /* Clean desc: replace newlines with spaces, full length (no truncation) */
    char *desc_clean = NULL;
    int desc_len = 0;
    if (si->desc && si->desc[0]) {
      desc_len = (int)strlen(si->desc);
      desc_clean = xmalloc((size_t)desc_len + 1);
      if (desc_clean) {
        for (int k = 0; k < desc_len; k++) {
          if (si->desc[k] == '\n' || si->desc[k] == '\r')
            desc_clean[k] = ' ';
          else
            desc_clean[k] = si->desc[k];
        }
        desc_clean[desc_len] = '\0';
      }
    }

    /* Prepare thought text: strip leading/trailing whitespace/newlines */
    const char *thought_start = si->thought;
    int tlen = 0;
    if (thought_start && thought_start[0]) {
      while (*thought_start == '\n' || *thought_start == '\r' ||
             *thought_start == ' ' || *thought_start == '\t')
        thought_start++;
      tlen = (int)strlen(thought_start);
      while (tlen > 0 && (thought_start[tlen - 1] == '\n' ||
                          thought_start[tlen - 1] == '\r' ||
                          thought_start[tlen - 1] == ' ' ||
                          thought_start[tlen - 1] == '\t'))
        tlen--;
    }

    /* Build link URI: subtask -> child reactR0.md, others -> ref#toolname */
    char link_uri[256] = "";
    if (si->child_dir)
      snprintf(link_uri, sizeof(link_uri), "%s/reactR0.md", si->child_dir);
    else if (si->ref)
      snprintf(link_uri, sizeof(link_uri), "%s#%s", si->ref, si->tool);

/* Build the step line */
/* Format:   RXSY [tool_name](ref#tool) `args` (Ns)
         *         Only tool_name is a hyperlink, args rendered as `code`
         *         Ref column is padded to 10 chars to fit R999S9999.
         *
         * If the text (desc or thought) is longer than the remaining
         * space to the end of the terminal, print it in full on a
         * continuation line underneath instead of truncating. */

/* Pad ref to fixed width (10 = fits "R999S9999") */
#define REF_COL_WIDTH 10
    char ref_pad[16] = "";
    if (si->ref)
      snprintf(ref_pad, sizeof(ref_pad), "%-*s", REF_COL_WIDTH, si->ref);
    else
      snprintf(ref_pad, sizeof(ref_pad), "%-*s", REF_COL_WIDTH, "");

    /* Pad tool name to max_tool_len for column alignment */
    char tool_pad[64];
    snprintf(tool_pad, sizeof(tool_pad), "%-*s", max_tool_len, si->tool);

/* Format HH:MM from timestamp (first column) */
#define TIME_COL_WIDTH 6        /* "HH:MM " */
    char time_col[8] = "     "; /* 5 spaces fallback */
    if (si->ts > 0) {
      time_t tt = (time_t)si->ts;
      struct tm *tm = localtime(&tt);
      if (tm)
        snprintf(time_col, sizeof(time_col), "%02d:%02d",
                 tm->tm_hour, tm->tm_min);
    }


/* Helper: emit the tool header line (without any text content) */
#define EMIT_TOOL_HEADER(with_elapsed) \
  do { \
    if (link_uri[0]) { \
      str_appendf(&md, "%s %s [%s](%s)%s\n", \
                  time_col, ref_pad, \
                  tool_pad, link_uri, \
                  (with_elapsed) ? elapsed_str : ""); \
    } else { \
      str_appendf(&md, "%s %s %s%s\n", \
                  time_col, ref_pad, \
                  tool_pad, \
                  (with_elapsed) ? elapsed_str : ""); \
    } \
  } while (0)

/* Helper: emit the tool header with inline text */
#define EMIT_TOOL_WITH_TEXT(text, with_elapsed) \
  do { \
    if (link_uri[0]) { \
      str_appendf(&md, "%s %s [%s](%s) `%s`%s\n", \
                  time_col, ref_pad, \
                  tool_pad, link_uri, \
                  (text), (with_elapsed) ? elapsed_str : ""); \
    } else { \
      str_appendf(&md, "%s %s %s `%s`%s\n", \
                  time_col, ref_pad, \
                  tool_pad, (text), \
                  (with_elapsed) ? elapsed_str : ""); \
    } \
  } while (0)

/* Helper: emit the tool header with inline thought (plain text, no backticks) */
#define EMIT_TOOL_WITH_THOUGHT(text, with_elapsed) \
  do { \
    if (link_uri[0]) { \
      str_appendf(&md, "%s %s [%s](%s) %s%s\n", \
                  time_col, ref_pad, \
                  tool_pad, link_uri, \
                  (text), (with_elapsed) ? elapsed_str : ""); \
    } else { \
      str_appendf(&md, "%s %s %s %s%s\n", \
                  time_col, ref_pad, \
                  tool_pad, (text), \
                  (with_elapsed) ? elapsed_str : ""); \
    } \
  } while (0)

/* Helper: emit thought text as a normal paragraph (using ~> prefix).
         * Each line of the thought becomes a separate ~> line.
         * If thought is a single long line, emit it as one ~> line
         * (md_render.c will word-wrap it). */
#define EMIT_THOUGHT_PARAGRAPH() \
  do { \
    const char *p = thought_start; \
    int remaining = tlen; \
    while (remaining > 0) { \
      /* Find next newline */ \
      const char *nl = NULL; \
      for (int k = 0; k < remaining; k++) { \
        if (p[k] == '\n' || p[k] == '\r') { \
          nl = p + k; \
          break; \
        } \
      } \
      if (nl) { \
        int line_len = (int)(nl - p); \
        if (line_len > 0) { \
          str_append_cstr(&md, "~> "); \
          str_append(&md, p, (size_t)line_len); \
          str_append_cstr(&md, "\n"); \
        } \
        p = nl + 1; \
        remaining = tlen - (int)(p - thought_start); \
        /* Skip \r\n pairs */ \
        if (remaining > 0 && *p == '\n') { \
          p++; \
          remaining--; \
        } \
      } else { \
        /* Last (or only) line */ \
        if (remaining > 0) { \
          str_append_cstr(&md, "~> "); \
          str_append(&md, p, (size_t)remaining); \
          str_append_cstr(&md, "\n"); \
        } \
        break; \
      } \
    } \
  } while (0)

    /* Thought always goes ABOVE the tool header as a green ~> paragraph */
    if (tlen > 0) {
      EMIT_THOUGHT_PARAGRAPH();
    }

    /* Tool header line with arguments (desc) inline.
     * Always keep desc on the same line as the tool name;
     * md_render.c word-wraps long code spans across multiple lines. */
    if (desc_clean) {
      EMIT_TOOL_WITH_TEXT(desc_clean, 1);
    } else {
      EMIT_TOOL_HEADER(1);
    }

#undef EMIT_TOOL_HEADER
#undef EMIT_TOOL_WITH_TEXT
#undef EMIT_TOOL_WITH_THOUGHT
#undef EMIT_THOUGHT_PARAGRAPH
#undef TIME_COL_WIDTH

    free(desc_clean);

    /* Preview: show for last step or explicitly expanded steps.
         * Plan tool shows full preview rendered as markdown, except
         * add_item which uses a compact inline description instead.
         * file_edit always shows its diff preview. */
    int is_plan_add = (strcmp(si->tool, "plan") == 0 &&
                       si->desc && si->desc[0]);
    int is_plan = (strcmp(si->tool, "plan") == 0 && !is_plan_add);
    int is_file_edit = (strcmp(si->tool, "file_edit") == 0);
    int show_preview = (is_last && !is_plan_add) || is_plan || is_file_edit;
    if (!show_preview && si->ref) {
      for (int ei = 0; ei < ui->expanded_count; ei++) {
        if (strcmp(ui->expanded_uris[ei], si->ref) == 0) {
          show_preview = 1;
          break;
        }
      }
    }

    if (show_preview && si->ref) {
      char rpath[NASH_PATH_MAX];
      path_join(rpath, sizeof(rpath), eff_dir, si->ref);

      if (is_plan) {
        /* Plan: read full file and render as markdown (no
                 * code fences, no line limit) so numbered steps
                 * display with proper formatting. */
        char *plan_text = slurp_file(rpath, NULL);
        if (plan_text) {
          str_append_cstr(&md, "\n");
          str_append_cstr(&md, plan_text);
          if (plan_text[0] &&
              plan_text[strlen(plan_text) - 1] != '\n')
            str_append_cstr(&md, "\n");
          str_append_cstr(&md, "\n");
          free(plan_text);
        }
      } else {
        FILE *cf = fopen(rpath, "r");
        if (cf) {
          if (is_file_edit)
            str_append_cstr(&md, "```diff\n");
          else
            str_append_cstr(&md, "```\n");
          char cbuf[NASH_PATH_MAX];
          int line_count = 0;
          size_t total = 0;
          size_t n;
          /* file_edit: show full diff without truncation */
          int max_lines = is_file_edit ? INT_MAX : 5;
          size_t max_bytes = is_file_edit ? SIZE_MAX : 8000;
          while ((n = fread(cbuf, 1, sizeof(cbuf) - 1, cf)) > 0 && total < max_bytes && line_count < max_lines) {
            cbuf[n] = '\0';
            for (size_t k = 0; k < n && line_count < max_lines; k++) {
              str_append(&md, &cbuf[k], 1);
              total++;
              if (cbuf[k] == '\n') line_count++;
            }
          }
          long file_sz = 0;
          fseek(cf, 0, SEEK_END);
          file_sz = ftell(cf);
          if (total < (size_t)file_sz)
            str_append_cstr(&md, "  ...\n");
          if (md.len > 0 && md.data[md.len - 1] != '\n')
            str_append_cstr(&md, "\n");
          str_append_cstr(&md, "```\n");
          fclose(cf);
        }
      }
    }
  }

  /* In-flight subtask indicator: show subtask dirs that exist on disk
   * but haven't been journaled yet (subtask tool_journal() only fires
   * after react_run() completes in tool_subtask.c).  Without this,
   * the parent's reactRX.md appears empty while a subtask runs. */
  {
    int sn = 0;
    char **snames = plan_subtask_names(eff_dir, &sn);
    if (snames) {
      for (int si = 0; si < sn; si++) {
        /* Skip subtasks already journaled in ANY react loop (not just
         * current).  Without this, completed subtasks from prior loops
         * show as "running..." in subsequent loops because the current
         * loop's steps[] has no child_dir entries for them. */
        int already_shown = 0;
        for (int j = 0; j < n_all_child_dirs; j++) {
          if (strcmp(all_child_dirs[j], snames[si]) == 0) {
            already_shown = 1;
            break;
          }
        }
        if (!already_shown) {
          /* Check that the subtask dir has a journal (is active/ran) */
          char sjpath[NASH_PATH_MAX];
          snprintf(sjpath, sizeof(sjpath), "%s/%s/journal.jsonl",
                   eff_dir, snames[si]);
          if (access(sjpath, F_OK) == 0) {
            str_appendf(&md, "~> [%s](%s/reactR0.md) running...\n",
                        snames[si], snames[si]);
          }
        }
      }
      plan_subtask_names_free(snames, sn);
    }
  }

  /* Streaming indicator if actively running — show live progress */
  if (ui->status == STATUS_RUNNING &&
      ui->current_react_loop == react_loop) {
    static const char spin[] = "|/-\\";
    char sc = spin[ui->spinner_phase % 4];
    ui->spinner_phase++;

    /* Build progress string based on streaming state */
    char progress[256];
    char *progress_dyn = NULL;           /* heap-allocated for long tool descriptions */
    const char *progress_ptr = progress; /* points to whichever buffer is active */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (ui->tool_executing) {
      /* Tool is running -- format to match completed step lines:
             * "HH:MM ref_pad  tool_pad  `args` (elapsed)" */
      double tool_elapsed = (now.tv_sec - ui->tool_start_time.tv_sec) +
                            (now.tv_nsec - ui->tool_start_time.tv_nsec) / 1e9;
      /* Extract action and description separately from tool_display
             * which has format "[step N] action: description" */
      const char *tool_name = "";
      const char *tool_args = "";
      char *tool_name_buf = NULL;
      if (ui->tool_display) {
        const char *br = strchr(ui->tool_display, ']');
        const char *after = (br && br[1] == ' ') ? br + 2 : ui->tool_display;
        /* after = "action: description" -- split on ": " */
        const char *sep = strstr(after, ": ");
        if (sep) {
          tool_name_buf = xstrndup(after, (size_t)(sep - after));
          tool_name = tool_name_buf ? tool_name_buf : after;
          tool_args = sep + 2;
        } else {
          tool_name = after;
        }
      }

      /* Format HH:MM from wall-clock start time */
      char time_col[8] = "     ";
      if (ui->tool_start_wallclock > 0) {
        struct tm *tm = localtime(&ui->tool_start_wallclock);
        if (tm)
          snprintf(time_col, sizeof(time_col), "%02d:%02d",
                   tm->tm_hour, tm->tm_min);
      }

      /* Ref alias column (predicted) */
      char ref_pad[16] = "";
      if (ui->tool_ref)
        snprintf(ref_pad, sizeof(ref_pad), "%-10s", ui->tool_ref);
      else
        snprintf(ref_pad, sizeof(ref_pad), "%-10s", "");

      /* Tool name padded to match completed step column width */
      int tname_len = (int)strlen(tool_name);
      int pad_len = max_tool_len > tname_len ? max_tool_len : tname_len;
      char tool_pad[64];
      snprintf(tool_pad, sizeof(tool_pad), "%-*s", pad_len, tool_name);

      /* Elapsed time string */
      char elapsed_buf[32], timeout_buf[32];
      fmt_duration(tool_elapsed, elapsed_buf, sizeof(elapsed_buf));
      char elapsed_str[80];
      if (ui->tool_timeout_secs > 0) {
        fmt_duration((double)ui->tool_timeout_secs,
                     timeout_buf, sizeof(timeout_buf));
        snprintf(elapsed_str, sizeof(elapsed_str),
                 " (%s/%s)", elapsed_buf, timeout_buf);
      } else {
        snprintf(elapsed_str, sizeof(elapsed_str),
                 " (%s)", elapsed_buf);
      }

      /* Emit line matching completed format:
             * "HH:MM ref_pad  tool_pad  `args` (elapsed)" */
      if (tool_args[0])
        asprintf(&progress_dyn, "%s %s %s `%s`%s",
                 time_col, ref_pad, tool_pad, tool_args, elapsed_str);
      else
        asprintf(&progress_dyn, "%s %s %s%s",
                 time_col, ref_pad, tool_pad, elapsed_str);
      if (progress_dyn)
        progress_ptr = progress_dyn;
      else
        snprintf(progress, sizeof(progress), "%s %s %s%s",
                 time_col, ref_pad, tool_pad, elapsed_str);
      free(tool_name_buf);
    } else if (ui->stream_first_token_seen && ui->stream_token_count > 0) {
      /* Tokens are flowing — show generation progress */
      double gen_elapsed = (now.tv_sec - ui->stream_first_token.tv_sec) +
                           (now.tv_nsec - ui->stream_first_token.tv_nsec) / 1e9;
      if (gen_elapsed > 0.1 && ui->stream_token_count > 1) {
        double tps = (ui->stream_token_count - 1) / gen_elapsed;
        snprintf(progress, sizeof(progress),
                 "generating... %d tokens (%.1f t/s)",
                 ui->stream_token_count, tps);
      } else {
        snprintf(progress, sizeof(progress),
                 "generating... %d tokens",
                 ui->stream_token_count);
      }
      /* Append tail preview of streamed content to status line */
      if (ui->stream_tokens && ui->stream_len > 0) {
        const char *sp = ui->stream_tokens;
        while (*sp == ' ' || *sp == '\n' || *sp == '\r' || *sp == '\t')
          sp++;
        if (*sp != '{') {
          int plen = (int)strlen(progress);
          int tail_max = 50;
          const char *src = ui->stream_tokens;
          int slen = ui->stream_len;
          int start = 0;
          int truncated = 0;
          if (slen > tail_max) {
            start = slen - tail_max;
            /* advance past any UTF-8 continuation bytes */
            while (start < slen &&
                   ((unsigned char)src[start] & 0xC0) == 0x80)
              start++;
            truncated = 1;
          }
          char tail[128];
          int ti = 0;
          if (truncated) {
            tail[ti++] = '.';
            tail[ti++] = '.';
            tail[ti++] = '.';
          }
          for (int i = start; i < slen &&
                              ti < (int)sizeof(tail) - 2;
               i++) {
            unsigned char c = (unsigned char)src[i];
            if (c == '\n' || c == '\r' || c == '\t')
              tail[ti++] = ' ';
            else if (c >= 0x20 || (c & 0x80))
              tail[ti++] = (char)c;
          }
          /* trim trailing spaces */
          while (ti > 0 && tail[ti - 1] == ' ')
            ti--;
          tail[ti] = '\0';
          if (ti > 0) {
            int avail = (int)sizeof(progress) - plen - 1;
            if (avail > 10)
              snprintf(progress + plen, (size_t)avail,
                       ": '%s'", tail);
          }
        }
      }
    } else {
      /* No tokens yet — prompt is being processed */
      double pp_elapsed = (now.tv_sec - ui->stream_step_start.tv_sec) +
                          (now.tv_nsec - ui->stream_step_start.tv_nsec) / 1e9;
      if (ui->prompt_progress_total > 0) {
        /* Server-reported progress (llama.cpp return_progress) */
        int pct = (int)(100.0 * ui->prompt_progress_processed /
                        ui->prompt_progress_total);
        if (pct > 100) pct = 100;
        if (pp_elapsed >= 0.5)
          snprintf(progress, sizeof(progress),
                   "prompt processing... %d%% (%.1fs)", pct, pp_elapsed);
        else
          snprintf(progress, sizeof(progress),
                   "prompt processing... %d%%", pct);
      } else if (pp_elapsed >= 0.5) {
        snprintf(progress, sizeof(progress),
                 "prompt processing... (%.1fs)", pp_elapsed);
      } else {
        snprintf(progress, sizeof(progress), "prompt processing...");
      }
    }

    if (ui->tool_executing) {
      /* Tool executing: progress_ptr already contains the full
             * formatted line matching completed step format */
      str_appendf(&md, "%s\n", progress_ptr);
    } else if (ui->max_steps > 0) {
      str_appendf(&md, "  %c %3d/%-3d %s\n",
                  sc, ui->current_step, ui->max_steps, progress_ptr);
    } else {
      str_appendf(&md, "  %c %3d     %s\n",
                  sc, ui->current_step, progress_ptr);
    }
    free(progress_dyn);
    if (!ui->tool_executing &&
        ui->stream_tokens && ui->stream_len > 0) {
      /* During tool execution, stream_tokens holds the tool command
             * text (set by TOOL_START handler) which is already shown in
             * the progress line above.  Showing it again in a code block
             * is redundant and adds extra lines that push the stats
             * footer off-screen, causing visible flicker as auto-scroll
             * oscillates between the two.
             *
             * Suppress display of raw JSON action objects (e.g.
             * {"thought":"","action":"file_read","path":"R1S31"}).
             * Local models emit the entire JSON response as streamed
             * tokens — showing it raw is ugly and distracting.
             * The progress indicator above still shows token count
             * and generation speed, giving the user feedback.
             * Only suppress content that looks like a JSON object
             * (starts with '{' after whitespace); genuine text
             * responses (thinking, errors) are still displayed. */
      const char *p = ui->stream_tokens;
      while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
      if (*p != '{') {
        str_append_cstr(&md, "```\n");
        str_append(&md, ui->stream_tokens, (size_t)ui->stream_len);
        str_append_cstr(&md, "\n```\n");
      }
    }
  }

  /* Token generation statistics footer.
   * For the active loop, prefer live stats (updated during streaming).
   * For past loops, use stats reconstructed from journal log entries. */
  {
    int is_current = (ui->current_react_loop == react_loop);
    int s_prompt = is_current ? ui->cum_prompt_tokens : j_prompt_tokens;
    int s_completion = is_current ? ui->cum_completion_tokens : j_completion_tokens;
    double s_gen = is_current ? ui->cum_predicted_per_second : j_gen_tps;
    double s_pp = is_current ? ui->cum_prompt_per_second : j_pp_tps;
    int s_calls = is_current ? ui->cum_llm_steps : j_llm_steps;
    double s_elapsed = 0;

    if (is_current) {
      s_elapsed = ui->react_total_elapsed;
    } else if (nsteps >= 2 && steps[nsteps - 1].ts > 0 && steps[0].ts > 0) {
      s_elapsed = steps[nsteps - 1].ts - steps[0].ts;
    }

    if (s_prompt > 0 || s_completion > 0) {
      str_append_cstr(&md, "\n---\n");

      str_appendf(&md, "\xf0\x9f\x93\x8a %d in \xe2\x86\x92 %d out",
                  s_prompt, s_completion);

      if (s_gen > 0)
        str_appendf(&md, " | gen %.0f t/s", s_gen);

      if (s_pp > 0)
        str_appendf(&md, " | pp %.0f t/s", s_pp);

      /* Start time HH:MM:SS from first step */
      if (nsteps > 0 && steps[0].ts > 0) {
        time_t t0 = (time_t)steps[0].ts;
        struct tm *tm0 = localtime(&t0);
        if (tm0) {
          char start_buf[16];
          strftime(start_buf, sizeof(start_buf), "%H:%M:%S", tm0);
          str_appendf(&md, " | start %s", start_buf);
        }
      }

      if (s_elapsed > 0) {
        char dur[32];
        fmt_duration(s_elapsed, dur, sizeof(dur));
        str_appendf(&md, " | total %s", dur);
      }

      if (s_calls > 1)
        str_appendf(&md, " | %d calls", s_calls);

      str_append_cstr(&md, "\n");
    }
  }

  /* user_ask: display full question in main pane */
  if (ui->status == STATUS_AWAITING_INPUT &&
      ui->current_react_loop == react_loop &&
      ui->user_ask_question && ui->user_ask_question[0]) {
    str_append_cstr(&md, "\n---\n\n");
    str_append_cstr(&md, "## \xf0\x9f\xa4\x94 Agent Question\n\n");
    str_append_cstr(&md, ui->user_ask_question);
    str_append_cstr(&md, "\n\n---\n");
    str_append_cstr(&md, "*Type your answer in the input bar below and press Enter*\n");
  }

  /* Free collected steps */
  for (int i = 0; i < nsteps; i++) {
    free(steps[i].tool);
    free(steps[i].desc);
    free(steps[i].thought);
    free(steps[i].ref);
    free(steps[i].compact_desc);
    free(steps[i].child_dir);
  }
  free(steps);
  free(query_text);
  for (int i = 0; i < n_all_child_dirs; i++)
    free(all_child_dirs[i]);
  free(all_child_dirs);

  char *md_str = str_steal(&md);
  char rpath[NASH_PATH_MAX];
  snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
           eff_dir, react_loop);
  write_md_file(rpath, md_str);
  free(md_str);
}

/* ═══════════════════════════════════════════════════════════
 *  View mode generators (F3-F6)
 *
 *  Each returns a malloc'd markdown string.  The dispatcher
 *  ui_state_generate_view_md() parses it into ui->doc.
 * ═══════════════════════════════════════════════════════════ */

/* ── F3: Plan ────────────────────────────────────────────── */
/* f3_* helpers removed - now using shared plan_link_for(), plan_subtask_names(),
 * plan_subtask_names_free(), plan_render_subtask_items() from tools.c */

/* Build an ancestor chain from root session dir down to eff_dir.
 * chain[0] = root session dir, chain[depth-1] = eff_dir.
 * Returns xstrdup'd array (caller frees each entry + array).
 * If eff_dir is not inside a subtask, *out_depth = 1 and chain[0] = eff_dir. */
static char **plan_ancestor_chain(const char *eff_dir, int *out_depth) {
  /* Collect dirs bottom-up: walk dirname while basename starts with "subtask_" */
  char **rev = NULL;
  int n = 0, cap = 0;
  char *cur = xstrdup(eff_dir);
  for (;;) {
    if (n == cap) {
      cap = cap ? cap * 2 : 4;
      char **tmp = xmalloc((size_t)cap * sizeof(char *));
      if (rev) { memcpy(tmp, rev, (size_t)n * sizeof(char *)); free(rev); }
      rev = tmp;
    }
    rev[n++] = cur;
    /* Check if cur's basename starts with "subtask_" */
    char *tmp_path = xstrdup(cur);
    char *base = basename(tmp_path);
    int is_subtask = (strncmp(base, "subtask_", 8) == 0);
    free(tmp_path);
    if (!is_subtask) break;
    /* Go up one level */
    char *tmp_path2 = xstrdup(cur);
    char *parent = dirname(tmp_path2);
    cur = xstrdup(parent);
    free(tmp_path2);
  }
  /* Reverse to get root-first order */
  char **chain = xmalloc((size_t)n * sizeof(char *));
  for (int i = 0; i < n; i++)
    chain[i] = rev[n - 1 - i];
  free(rev);
  *out_depth = n;
  return chain;
}

/* Render a plan level into md with the given indent prefix.
 * indent: number of leading spaces for each line (0 for root level).
 * Subtask plans are rendered recursively inline under the parent step
 * that spawned them, indented by 4 additional spaces per nesting level.
 * Traverses each subtask's journal to discover its plan and recurses. */
static void render_plan_level(str_t *md, const char *dir, int indent) {
  if (indent > 20) return;  /* recursion depth limit */

  cJSON *root = plan_replay_journal_dir(dir);
  cJSON *steps = root ? cJSON_GetObjectItem(root, "steps") : NULL;
  int active_step = root ? json_int(root, "active_step", 0) : 0;
  int has_steps = (steps && cJSON_IsArray(steps) &&
                   cJSON_GetArraySize(steps) > 0);

  if (has_steps) {
    int total = cJSON_GetArraySize(steps);
    int done_count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, steps) {
      if (cJSON_IsTrue(cJSON_GetObjectItem(item, "done"))) done_count++;
    }
    /* Header - only at root level (indent==0) */
    if (indent == 0)
      str_appendf(md, "## Plan Progress (%d/%d)\n\n", done_count, total);

    cJSON *links = plan_subtask_links(dir);
    int idx = 0;
    cJSON_ArrayForEach(item, steps) {
      idx++;
      int is_done = cJSON_IsTrue(cJSON_GetObjectItem(item, "done"));
      int is_stale = cJSON_IsTrue(cJSON_GetObjectItem(item, "stale"));
      const char *text = json_str(item, "text");
      const char *evidence = json_str(item, "evidence");
      const char *marker;
      if (is_done && is_stale) marker = "~";
      else if (is_done) marker = "x";
      else if (idx == active_step) marker = ">";
      else marker = " ";
      /* Indent */
      for (int sp = 0; sp < indent; sp++) str_append_cstr(md, " ");
      str_appendf(md, "%d. [%s] %s", idx, marker, text ? text : "?");
      if (is_done && evidence && evidence[0])
        str_appendf(md, " (%s)", evidence);
      if (is_stale)
        str_append_cstr(md, " (STALE)");
      str_append_cstr(md, "\n");

      /* Recursively render subtask plans linked to this step */
      {
        int sn = 0;
        char **snames = plan_subtask_names(dir, &sn);
        if (snames) {
          for (int si = 0; si < sn; si++) {
            if (plan_link_for(links, snames[si]) != idx) continue;
            char child_dir[NASH_PATH_MAX];
            snprintf(child_dir, sizeof(child_dir), "%s/%s", dir, snames[si]);
            /* Check if child has a formal plan; if not, show a
             * one-liner so the subtask is still visible. */
            cJSON *croot = plan_replay_journal_dir(child_dir);
            cJSON *csteps = croot ? cJSON_GetObjectItem(croot, "steps") : NULL;
            int child_has_plan = (csteps && cJSON_IsArray(csteps) &&
                                  cJSON_GetArraySize(csteps) > 0);
            cJSON_Delete(croot);
            if (child_has_plan) {
              render_plan_level(md, child_dir, indent + 4);
            } else {
              const char *ref = plan_link_ref(links, snames[si]);
              for (int sp = 0; sp < indent + 4; sp++)
                str_append_cstr(md, " ");
              str_appendf(md, "[%s](%s/reactR0.md)", snames[si], child_dir);
              if (ref && ref[0])
                str_appendf(md, " [%s](%s#subtask)", ref, ref);
              str_append_cstr(md, "\n");
            }
          }
          plan_subtask_names_free(snames, sn);
        }
      }

      /* Render unlinked subtasks under the active step so in-flight
       * subtasks appear where they logically belong, not at the end. */
      if (idx == active_step) {
        int usn = 0;
        char **unames = plan_subtask_names(dir, &usn);
        if (unames) {
          for (int ui = 0; ui < usn; ui++) {
            if (plan_link_for(links, unames[ui]) != 0) continue;
            char uchild_dir[NASH_PATH_MAX];
            snprintf(uchild_dir, sizeof(uchild_dir), "%s/%s", dir, unames[ui]);
            cJSON *ucroot = plan_replay_journal_dir(uchild_dir);
            cJSON *ucsteps = ucroot ? cJSON_GetObjectItem(ucroot, "steps") : NULL;
            int uchild_has_plan = (ucsteps && cJSON_IsArray(ucsteps) &&
                                   cJSON_GetArraySize(ucsteps) > 0);
            cJSON_Delete(ucroot);
            if (uchild_has_plan) {
              render_plan_level(md, uchild_dir, indent + 4);
            } else {
              const char *ref = plan_link_ref(links, unames[ui]);
              for (int sp = 0; sp < indent + 4; sp++)
                str_append_cstr(md, " ");
              str_appendf(md, "[%s](%s/reactR0.md)", unames[ui], uchild_dir);
              if (ref && ref[0])
                str_appendf(md, " [%s](%s#subtask)", ref, ref);
              str_append_cstr(md, "\n");
            }
          }
          plan_subtask_names_free(unames, usn);
        }
      }
    }

    /* Fallback: if active_step is 0 (all steps done), render any
     * remaining unlinked subtasks after the last step so they are
     * still visible rather than silently dropped. */
    if (active_step == 0) {
      int usn = 0;
      char **unames = plan_subtask_names(dir, &usn);
      if (unames) {
        for (int ui = 0; ui < usn; ui++) {
          if (plan_link_for(links, unames[ui]) != 0) continue;
          char uchild_dir[NASH_PATH_MAX];
          snprintf(uchild_dir, sizeof(uchild_dir), "%s/%s", dir, unames[ui]);
          cJSON *ucroot = plan_replay_journal_dir(uchild_dir);
          cJSON *ucsteps = ucroot ? cJSON_GetObjectItem(ucroot, "steps") : NULL;
          int uchild_has_plan = (ucsteps && cJSON_IsArray(ucsteps) &&
                                 cJSON_GetArraySize(ucsteps) > 0);
          cJSON_Delete(ucroot);
          if (uchild_has_plan) {
            render_plan_level(md, uchild_dir, indent + 4);
          } else {
            const char *ref = plan_link_ref(links, unames[ui]);
            for (int sp = 0; sp < indent + 4; sp++)
              str_append_cstr(md, " ");
            str_appendf(md, "[%s](%s/reactR0.md)", unames[ui], uchild_dir);
            if (ref && ref[0])
              str_appendf(md, " [%s](%s#subtask)", ref, ref);
            str_append_cstr(md, "\n");
          }
        }
        plan_subtask_names_free(unames, usn);
      }
    }
    cJSON_Delete(links);
  } else {
    /* No plan steps at this level - still check for subtask dirs
     * that may have their own plans (e.g. in-flight subtasks whose
     * parent hasn't journaled them yet, or subtasks spawned at a
     * level that never created a formal plan). */
    int sn = 0;
    char **snames = plan_subtask_names(dir, &sn);
    if (snames) {
      for (int si = 0; si < sn; si++) {
        char child_dir[NASH_PATH_MAX];
        snprintf(child_dir, sizeof(child_dir), "%s/%s", dir, snames[si]);
        render_plan_level(md, child_dir, indent);
      }
      plan_subtask_names_free(snames, sn);
    }
  }

  cJSON_Delete(root);
}

static char *generate_working_mem_md(ui_state_t *ui) {
  const char *eff_dir = ui->playbook_session_dir
                          ? ui->playbook_session_dir
                          : ui->session_dir;
  str_t md = str_new(4096);
  str_append_cstr(&md, "# Plan\n\n");

  /* Build ancestor chain to find root session dir, then render the
   * full plan hierarchy recursively from root.  render_plan_level
   * traverses each subtask's journal and inlines its plan steps
   * under the parent step that spawned it, indented by depth. */
  {
    int depth = 0;
    char **chain = plan_ancestor_chain(eff_dir, &depth);

    /* Always render from root - recursion handles all subtask levels */
    render_plan_level(&md, chain[0], 0);
    str_append_cstr(&md, "\n");

    for (int i = 0; i < depth; i++) free(chain[i]);
    free(chain);
  }

  /* Scratchpad summary (section names + sizes, not full content) */
  {
    scratchpad_t sp;
    scratchpad_init(&sp);
    if (scratchpad_load(&sp, eff_dir) == 0 && sp.count > 0) {
      str_append_cstr(&md, "## Scratchpad Summary\n\n");
      size_t total = scratchpad_total_size(&sp);
      str_appendf(&md, "%d section(s), %.1fK chars total\n\n",
                  sp.count, (double)total / 1024.0);
      for (int i = 0; i < sp.count; i++) {
        size_t slen = sp.sections[i].content
                        ? strlen(sp.sections[i].content) : 0;
        str_appendf(&md, "- **%s** (priority %d): %zuB\n",
                    sp.sections[i].name ? sp.sections[i].name : "(unnamed)",
                    sp.sections[i].priority, slen);
      }
      str_append_cstr(&md, "\n");
    }
    scratchpad_free(&sp);
  }

  /* Session stats summary */
  if (ui->cum_llm_steps > 0) {
    str_append_cstr(&md, "## Session Stats\n\n");
    str_appendf(&md, "- Steps: %d/%d (react loop R%d)\n",
                ui->current_step, ui->max_steps,
                ui->current_react_loop);
    str_appendf(&md, "- Prompt tokens: %d\n", ui->cum_prompt_tokens);
    str_appendf(&md, "- Completion tokens: %d\n", ui->cum_completion_tokens);
    if (ui->context_size > 0 && ui->context_used > 0) {
      double pct = 100.0 * ui->context_used / ui->context_size;
      str_appendf(&md, "- Context: %d%% (%dK / %dK)\n",
                  (int)pct, ui->context_used / 1000,
                  ui->context_size / 1000);
    }
    str_append_cstr(&md, "\n");
  }

  return str_steal(&md);
}

/* ── F4: Scratchpad (full content browser) ──────────────── */

static char *generate_scratchpad_md(ui_state_t *ui) {
  const char *eff_dir = ui->playbook_session_dir
                          ? ui->playbook_session_dir
                          : ui->session_dir;
  str_t md = str_new(8192);

  scratchpad_t sp;
  scratchpad_init(&sp);
  if (scratchpad_load(&sp, eff_dir) != 0 || sp.count == 0) {
    str_append_cstr(&md, "# Scratchpad\n\n*No scratchpad data*\n");
    scratchpad_free(&sp);
    return str_steal(&md);
  }

  size_t total = scratchpad_total_size(&sp);
  str_appendf(&md, "# Scratchpad (%d sections, %.1fK chars)\n\n",
              sp.count, (double)total / 1024.0);

  /* Render sections in priority order (already sorted by scratchpad_load) */
  for (int i = 0; i < sp.count; i++) {
    scratchpad_section_t *sec = &sp.sections[i];
    str_appendf(&md, "## %s (priority %d)\n\n",
                sec->name ? sec->name : "(unnamed)",
                sec->priority);
    if (sec->content && sec->content[0]) {
      str_append_cstr(&md, sec->content);
      if (sec->content[strlen(sec->content) - 1] != '\n')
        str_append_cstr(&md, "\n");
    } else {
      str_append_cstr(&md, "*(empty)*\n");
    }
    str_append_cstr(&md, "\n");
  }

  scratchpad_free(&sp);
  return str_steal(&md);
}

/* ── F5: Timeline (journal trace) ───────────────────────── */

static char *generate_timeline_md(ui_state_t *ui) {
  const char *eff_dir = ui->playbook_session_dir
                          ? ui->playbook_session_dir
                          : ui->session_dir;
  str_t md = str_new(4096);
  str_append_cstr(&md, "# Timeline\n\n");

  char jpath[NASH_PATH_MAX];
  snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", eff_dir);
  FILE *f = fopen(jpath, "r");
  if (!f) {
    str_append_cstr(&md, "*No journal data*\n");
    return str_steal(&md);
  }

  char line[NASH_LINE_MAX];
  double first_ts = 0;
  int event_count = 0;

  while (fgets(line, sizeof(line), f)) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;

    double ts = 0;
    cJSON *ts_j = cJSON_GetObjectItem(entry, "ts");
    if (ts_j) ts = cJSON_IsString(ts_j) ? atof(ts_j->valuestring)
                                         : ts_j->valuedouble;
    if (first_ts == 0 && ts > 0) first_ts = ts;

    const char *tool = json_str(entry, "tool");
    int step = json_int(entry, "step", 0);
    int react_loop = json_int(entry, "react_loop", 0);

    /* Filter: only show plan ops, query starts, done events,
     * and context compaction markers */
    int show = 0;
    const char *event_desc = NULL;
    char desc_buf[512];

    if (tool && strcmp(tool, "plan") == 0) {
      show = 1;
      cJSON *params = cJSON_GetObjectItem(entry, "params");
      const char *op = params ? json_str(params, "op") : NULL;
      if (op)
        snprintf(desc_buf, sizeof(desc_buf), "plan %s", op);
      else
        snprintf(desc_buf, sizeof(desc_buf), "plan update");
      event_desc = desc_buf;
    } else if (tool && strcmp(tool, "done") == 0) {
      show = 1;
      event_desc = "done() called";
    } else if (tool && strcmp(tool, "user_ask") == 0) {
      show = 1;
      event_desc = "user_ask";
    } else if (tool && strcmp(tool, "query") == 0) {
      /* Query events - tool is "query", text is in params.text */
      show = 1;
      cJSON *params = cJSON_GetObjectItem(entry, "params");
      const char *q = params ? json_str(params, "text") : NULL;
      if (q && q[0]) {
        size_t qlen = strlen(q);
        if (qlen > 80) {
          snprintf(desc_buf, sizeof(desc_buf), "Query: %.77s...", q);
        } else {
          snprintf(desc_buf, sizeof(desc_buf), "Query: %s", q);
        }
        event_desc = desc_buf;
      } else {
        event_desc = "Query started";
      }
    } else if (tool && strcmp(tool, "compaction") == 0) {
      /* Context compaction markers */
      show = 1;
      event_desc = "Context compacted";
    }

    if (show && event_desc) {
      double elapsed = (ts > 0 && first_ts > 0) ? (ts - first_ts) : 0;
      int mins = (int)(elapsed / 60);
      int secs = (int)(elapsed) % 60;
      str_appendf(&md, "`%02d:%02d`  R%d/S%d  %s\n",
                  mins, secs, react_loop, step, event_desc);
      event_count++;
    }

    cJSON_Delete(entry);
  }
  fclose(f);

  if (event_count == 0)
    str_append_cstr(&md, "*No plan events recorded*\n");

  return str_steal(&md);
}

/* ── F6: Metrics (token/performance dashboard) ──────────── */

static char *generate_metrics_md(ui_state_t *ui) {
  str_t md = str_new(2048);
  str_append_cstr(&md, "# Session Metrics\n\n");

  /* Current react loop stats */
  str_appendf(&md, "## React Loop R%d\n\n", ui->current_react_loop);

  str_appendf(&md, "| Metric | Value |\n");
  str_appendf(&md, "|--------|-------|\n");
  str_appendf(&md, "| Steps | %d / %d |\n",
              ui->current_step, ui->max_steps);
  str_appendf(&md, "| Prompt tokens | %d |\n", ui->cum_prompt_tokens);
  str_appendf(&md, "| Completion tokens | %d |\n", ui->cum_completion_tokens);
  int total_tok = ui->cum_prompt_tokens + ui->cum_completion_tokens;
  str_appendf(&md, "| Total tokens | %d |\n", total_tok);

  if (ui->context_size > 0) {
    double pct = ui->context_used > 0
                   ? 100.0 * ui->context_used / ui->context_size
                   : 0;
    str_appendf(&md, "| Context usage | %d%% (%dK / %dK) |\n",
                (int)pct, ui->context_used / 1000,
                ui->context_size / 1000);
  }

  if (ui->cum_predicted_per_second > 0)
    str_appendf(&md, "| Gen speed | %.1f t/s |\n",
                ui->cum_predicted_per_second);
  if (ui->cum_prompt_per_second > 0)
    str_appendf(&md, "| Prompt speed | %.0f t/s |\n",
                ui->cum_prompt_per_second);
  if (ui->react_total_elapsed > 0) {
    int mins = (int)(ui->react_total_elapsed / 60);
    int secs = (int)(ui->react_total_elapsed) % 60;
    str_appendf(&md, "| Wall time | %dm %ds |\n", mins, secs);
  }
  str_appendf(&md, "| LLM calls | %d |\n", ui->cum_llm_steps);
  str_append_cstr(&md, "\n");

  /* Model info */
  if (ui->model_name && ui->model_name[0]) {
    str_appendf(&md, "## Model\n\n");
    str_appendf(&md, "**%s**", ui->model_name);
    if (ui->context_size > 0)
      str_appendf(&md, " (%dK context)", ui->context_size / 1000);
    str_append_cstr(&md, "\n\n");
  }

  /* Per-tool aggregate stats from journal.
   * Journal entries have: react_loop, step, ts(string), tool, params, ref,
   * size, lines, failed, error, tc_id.
   * Token data lives in log entries with message matching
   * "[provider/complete] final stats: prompt_tokens=N ... completion_tokens=N".
   * We correlate log entries with tool calls by step number,
   * then aggregate by tool name. */
  const char *eff_dir = ui->playbook_session_dir
                          ? ui->playbook_session_dir
                          : ui->session_dir;
  char jpath[NASH_PATH_MAX];
  snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", eff_dir);
  FILE *f = fopen(jpath, "r");
  if (f) {
    /* First pass: collect per-step token stats from provider log entries */
    typedef struct { int ptok; int ctok; double gen_speed; } step_tokens_t;
    step_tokens_t step_tok[256] = {{0}}; /* indexed by step, capped at 256 */

    char line[NASH_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
      cJSON *entry = cJSON_Parse(line);
      if (!entry) continue;
      int loop = json_int(entry, "react_loop", 0);
      if (loop != ui->current_react_loop) {
        cJSON_Delete(entry);
        continue;
      }
      const char *tool = json_str(entry, "tool");
      int step = json_int(entry, "step", 0);
      /* Parse provider/complete log messages for token stats */
      if (tool && strcmp(tool, "log") == 0 && step >= 0 && step < 256) {
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        const char *msg = params ? json_str(params, "message") : NULL;
        if (msg && strstr(msg, "[provider/complete]")) {
          int pt = 0, ct = 0;
          double gs = 0;
          const char *p;
          if ((p = strstr(msg, "prompt_tokens=")) != NULL)
            pt = atoi(p + 14);
          if ((p = strstr(msg, "completion_tokens=")) != NULL)
            ct = atoi(p + 18);
          if ((p = strstr(msg, "gen=")) != NULL)
            gs = atof(p + 4);
          step_tok[step].ptok = pt;
          step_tok[step].ctok = ct;
          step_tok[step].gen_speed = gs;
        }
      }
      cJSON_Delete(entry);
    }

    /* Second pass: aggregate tool calls by tool name + collect modified files */
    typedef struct {
      char name[64];
      int calls;
      long total_size;
      long total_ptok;
      long total_ctok;
      double total_gen_speed;
      int gen_count;       /* how many steps had gen_speed > 0 */
    } tool_agg_t;
    #define MAX_TOOLS 64
    tool_agg_t tools[MAX_TOOLS];
    memset(tools, 0, sizeof(tools));
    int tool_count = 0;

    typedef struct {
      char path[512];
      int last_step;
      int count;
    } mod_file_t;
    #define MAX_MOD_FILES 64
    mod_file_t mod_files[MAX_MOD_FILES];
    memset(mod_files, 0, sizeof(mod_files));
    int mod_file_count = 0;

    rewind(f);
    while (fgets(line, sizeof(line), f)) {
      cJSON *entry = cJSON_Parse(line);
      if (!entry) continue;
      int loop = json_int(entry, "react_loop", 0);
      if (loop != ui->current_react_loop) {
        cJSON_Delete(entry);
        continue;
      }
      const char *tool = json_str(entry, "tool");
      int step = json_int(entry, "step", 0);
      /* Skip internal entries: log, system, ctx:*, spec, memory_context */
      if (!tool || step < 1) { cJSON_Delete(entry); continue; }
      if (strcmp(tool, "log") == 0 || strcmp(tool, "system") == 0 ||
          strcmp(tool, "spec") == 0 || strcmp(tool, "memory_context") == 0 ||
          strncmp(tool, "ctx:", 4) == 0) {
        cJSON_Delete(entry);
        continue;
      }

      /* Find or create aggregate slot for this tool */
      int idx = -1;
      for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, tool) == 0) { idx = i; break; }
      }
      if (idx < 0 && tool_count < MAX_TOOLS) {
        idx = tool_count++;
        snprintf(tools[idx].name, sizeof(tools[idx].name), "%s", tool);
      }
      if (idx >= 0) {
        tools[idx].calls++;
        tools[idx].total_size += json_int(entry, "size", 0);
        if (step >= 0 && step < 256) {
          tools[idx].total_ptok += step_tok[step].ptok;
          tools[idx].total_ctok += step_tok[step].ctok;
          if (step_tok[step].gen_speed > 0) {
            tools[idx].total_gen_speed += step_tok[step].gen_speed;
            tools[idx].gen_count++;
          }
        }
      }

      /* Collect modified files from file_write/file_edit entries */
      if (strcmp(tool, "file_write") == 0 || strcmp(tool, "file_edit") == 0) {
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        const char *fpath = params ? json_str(params, "path") : NULL;
        if (fpath && fpath[0]) {
          int midx = -1;
          for (int i = 0; i < mod_file_count; i++) {
            if (strcmp(mod_files[i].path, fpath) == 0) { midx = i; break; }
          }
          if (midx >= 0) {
            mod_files[midx].count++;
            if (step > mod_files[midx].last_step)
              mod_files[midx].last_step = step;
          } else if (mod_file_count < MAX_MOD_FILES) {
            midx = mod_file_count++;
            snprintf(mod_files[midx].path, sizeof(mod_files[midx].path),
                     "%s", fpath);
            mod_files[midx].last_step = step;
            mod_files[midx].count = 1;
          }
        }
      }

      cJSON_Delete(entry);
    }
    fclose(f);

    /* Sort by total tokens (prompt+completion) descending */
    for (int i = 0; i < tool_count - 1; i++) {
      for (int j = i + 1; j < tool_count; j++) {
        long ti = tools[i].total_ptok + tools[i].total_ctok;
        long tj = tools[j].total_ptok + tools[j].total_ctok;
        if (tj > ti) {
          tool_agg_t tmp = tools[i];
          tools[i] = tools[j];
          tools[j] = tmp;
        }
      }
    }

    /* Render per-tool aggregate table */
    str_append_cstr(&md, "## Per-Tool Breakdown\n\n");
    str_appendf(&md, "| Tool | Calls | Size | Tokens | Avg Gen |\n");
    str_appendf(&md, "|------|-------|------|--------|---------|\n");

    long grand_size = 0, grand_ptok = 0, grand_ctok = 0;
    int grand_calls = 0;

    for (int i = 0; i < tool_count; i++) {
      tool_agg_t *t = &tools[i];
      grand_calls += t->calls;
      grand_size += t->total_size;
      grand_ptok += t->total_ptok;
      grand_ctok += t->total_ctok;

      char sz_buf[32];
      if (t->total_size >= 1000)
        snprintf(sz_buf, sizeof(sz_buf), "%ldK", t->total_size / 1000);
      else
        snprintf(sz_buf, sizeof(sz_buf), "%ld", t->total_size);

      char tok_buf[32] = "-";
      if (t->total_ptok > 0 || t->total_ctok > 0)
        snprintf(tok_buf, sizeof(tok_buf), "%ld+%ld",
                 t->total_ptok, t->total_ctok);

      char gen_buf[16] = "-";
      if (t->gen_count > 0)
        snprintf(gen_buf, sizeof(gen_buf), "%.0f t/s",
                 t->total_gen_speed / t->gen_count);

      str_appendf(&md, "| %s | %d | %s | %s | %s |\n",
                  t->name, t->calls, sz_buf, tok_buf, gen_buf);
    }

    /* Totals row */
    if (tool_count > 0) {
      char gsz[32], gtok[32];
      if (grand_size >= 1000)
        snprintf(gsz, sizeof(gsz), "%ldK", grand_size / 1000);
      else
        snprintf(gsz, sizeof(gsz), "%ld", grand_size);
      if (grand_ptok > 0 || grand_ctok > 0)
        snprintf(gtok, sizeof(gtok), "%ld+%ld", grand_ptok, grand_ctok);
      else
        snprintf(gtok, sizeof(gtok), "-");
      str_appendf(&md, "| **Total** | **%d** | **%s** | **%s** | ||\n",
                  grand_calls, gsz, gtok);
    } else {
      str_append_cstr(&md, "| - | *no tool calls yet* | - | - | - |\n");
    }
    str_append_cstr(&md, "\n");

    /* Modified files section */
    if (mod_file_count > 0) {
      /* Sort by last_step descending (most recently modified first) */
      for (int i = 0; i < mod_file_count - 1; i++) {
        for (int j = i + 1; j < mod_file_count; j++) {
          if (mod_files[j].last_step > mod_files[i].last_step) {
            mod_file_t tmp = mod_files[i];
            mod_files[i] = mod_files[j];
            mod_files[j] = tmp;
          }
        }
      }

      str_append_cstr(&md, "## Modified Files\n\n");
      str_append_cstr(&md, "| File | Edits | Last Step |\n");
      str_append_cstr(&md, "|------|-------|-----------|\n");

      for (int i = 0; i < mod_file_count; i++) {
        /* Show dir/basename for context */
        const char *display = mod_files[i].path;
        const char *slash = strrchr(display, '/');
        if (slash && slash != display) {
          const char *prev = slash - 1;
          while (prev > display && *prev != '/') prev--;
          if (*prev == '/') prev++;
          display = prev;
        }
        str_appendf(&md, "| %s | %d | %d |\n",
                    display, mod_files[i].count, mod_files[i].last_step);
      }
      str_append_cstr(&md, "\n");
    }
  }

  return str_steal(&md);
}

/* ── Dispatcher: generate view-specific markdown ─────────── */

void ui_state_generate_view_md(ui_state_t *ui) {
  if (!ui || !ui->session_dir) return;

  char *md_str = NULL;
  switch (ui->view_mode) {
    case VIEW_WORKING_MEM: md_str = generate_working_mem_md(ui); break;
    case VIEW_SCRATCHPAD:  md_str = generate_scratchpad_md(ui);  break;
    case VIEW_TIMELINE:    md_str = generate_timeline_md(ui);    break;
    case VIEW_METRICS:     md_str = generate_metrics_md(ui);     break;
    default: return; /* VIEW_STREAM uses file-based pipeline */
  }

  if (!md_str) return;

  md_doc_free(ui->doc);
  ui->doc = md_parse(md_str);
  free(md_str);

  /* Clamp cursor */
  if (ui->doc && ui->cursor_link >= ui->doc->link_count)
    ui->cursor_link = ui->doc->link_count > 0 ? ui->doc->link_count - 1 : 0;

  ui->dirty = 1;
}
