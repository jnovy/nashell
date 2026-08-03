#include "llm.h"
#include "provider.h"
#include "str.h"
#include "tui.h"
#include "nash_log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> /* sqrtf, expf — for belief entropy probe */

/* ── Chat management ─────────────────────────────────────────── */

llm_chat_t *llm_chat_new(void) {
  llm_chat_t *c = xcalloc(1, sizeof(*c));
  c->cap_msgs = 32;
  c->msgs = xcalloc(c->cap_msgs, sizeof(llm_msg_t));
  return c;
}

/* Free all heap fields of a single message (but not the struct itself). */
void llm_msg_free_fields(llm_msg_t *m) {
  free(m->role);
  free(m->content);
  free(m->tool_call_id);
  free(m->tool_calls_json);
  free(m->tool_call_id_outbound);
  free(m->store_alias);
  free(m->tool_name);
  free(m->tool_path);
}

/* Ensure the msgs array has room for at least one more entry.
 * Returns 0 on success, -1 on allocation failure. */
static int llm_chat_ensure_capacity(llm_chat_t *chat) {
  if (chat->n_msgs < chat->cap_msgs) return 0;
  int new_cap = chat->cap_msgs * 2;
  if (safe_realloc((void **)&chat->msgs, (size_t)new_cap * sizeof(llm_msg_t))) return -1;
  chat->cap_msgs = new_cap;
  return 0;
}

void llm_chat_free(llm_chat_t *chat) {
  if (!chat) return;
  for (int i = 0; i < chat->n_msgs; i++)
    llm_msg_free_fields(&chat->msgs[i]);
  free(chat->msgs);
  free(chat->last_tool_call_id);
  free(chat->last_tool_calls_json);
  free(chat);
}

int llm_chat_add(llm_chat_t *chat, const char *role, const char *content) {
  if (llm_chat_ensure_capacity(chat) != 0) return -1;
  llm_msg_t *m = &chat->msgs[chat->n_msgs];
  memset(m, 0, sizeof(*m));
  m->role = xstrdup(role);
  m->content = xstrdup(content);
  m->content_len = strlen(m->content);
  chat->total_chars += (long)m->content_len;
  chat->n_msgs++;
  return 0;
}


/* Auto-assign importance based on message type (Harness-1 §3.2).
 * Called by all message-adding functions to ensure consistent tagging. */
static llm_msg_importance_t llm_importance_for_type(llm_msg_type_t type) {
  switch (type) {
    case LLM_MSG_SYSTEM:
    case LLM_MSG_USER_QUERY:
    case LLM_MSG_SCRATCHPAD:
      return LLM_MSG_IMPORTANCE_CRITICAL;
    /* D2 FIX: EVICTION_SUMMARY demoted from CRITICAL to HIGH.
         * Breadcrumb indices are useful but not irreplaceable — they
         * should not consume the same budget tier as the system prompt
         * and user query. They're removed at the start of each eviction
         * cycle anyway, so CRITICAL protection was only relevant between
         * evictions where it wasted context budget. */
    case LLM_MSG_EVICTION_SUMMARY:
    case LLM_MSG_PINNED:
    case LLM_MSG_PREV_RESULT:
      return LLM_MSG_IMPORTANCE_HIGH;
    /* Plan-then-shed preamble: these inform planning but become
         * dead weight after plan() executes. Start NORMAL so they
         * survive initial eviction passes; react_degrade_preamble()
         * downgrades them to LOW after plan(). */
    case LLM_MSG_MEMORY_INDEX:
    case LLM_MSG_TEMPORAL:
    case LLM_MSG_EPISODIC:
    case LLM_MSG_SKILLS:
    case LLM_MSG_LESSONS:
    case LLM_MSG_STRATEGIES:
    case LLM_MSG_ANTIPATTERNS:
      return LLM_MSG_IMPORTANCE_NORMAL;
    case LLM_MSG_ERROR:
    case LLM_MSG_MEMORY_HINT:
    case LLM_MSG_REPO_MAP:
      return LLM_MSG_IMPORTANCE_LOW;
    case LLM_MSG_TOOL_RESULT:
    case LLM_MSG_GENERIC:
    case LLM_MSG_THINKING:
    default:
      return LLM_MSG_IMPORTANCE_NORMAL;
  }
}

/* Add a typed message — sets msg_type for structured routing. */
int llm_chat_add_typed(llm_chat_t *chat, const char *role,
                       const char *content, llm_msg_type_t type) {
  if (llm_chat_ensure_capacity(chat) != 0) return -1;
  llm_msg_t *m = &chat->msgs[chat->n_msgs];
  memset(m, 0, sizeof(*m));
  m->role = xstrdup(role);
  m->content = xstrdup(content);
  m->content_len = strlen(m->content);
  chat->total_chars += (long)m->content_len;
  m->msg_type = type;
  m->importance = llm_importance_for_type(type);
  chat->n_msgs++;
  return 0;
}

/* Add a typed message with printf-style formatting.
 * Handles the alloc + snprintf + add_typed + free pattern internally. */
int llm_chat_add_formatted(llm_chat_t *chat, const char *role,
                           llm_msg_type_t type,
                           const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0) {
    va_end(ap2);
    return -1;
  }
  char *buf = xmalloc((size_t)needed + 1);
  vsnprintf(buf, (size_t)needed + 1, fmt, ap2);
  va_end(ap2);
  int rc = llm_chat_add_typed(chat, role, buf, type);
  free(buf);
  return rc;
}

/* Remove all messages of a given type. Returns count removed. */
int llm_chat_remove_by_type(llm_chat_t *chat, llm_msg_type_t type) {
  return llm_chat_remove_by_types(chat, &type, 1);
}

/* D1 FIX: Single-pass removal of multiple message types.
 * Avoids 3× O(n) scanning when removing SCRATCHPAD + EVICTION_SUMMARY + MEMORY_HINT. */
int llm_chat_remove_by_types(llm_chat_t *chat,
                             const llm_msg_type_t *types, int n_types) {
  if (!chat || !types || n_types <= 0) return 0;
  int removed = 0;
  int dst = 0;
  for (int src = 0; src < chat->n_msgs; src++) {
    int match = 0;
    for (int t = 0; t < n_types; t++) {
      if (chat->msgs[src].msg_type == types[t]) {
        match = 1;
        break;
      }
    }
    if (match) {
      chat->total_chars -= (long)chat->msgs[src].content_len;
      llm_msg_free_fields(&chat->msgs[src]);
      removed++;
    } else {
      if (dst != src)
        chat->msgs[dst] = chat->msgs[src];
      dst++;
    }
  }
  chat->n_msgs = dst;
  return removed;
}

/* Find the first message of a given type. Returns index or -1. */
int llm_chat_find_by_type(llm_chat_t *chat, llm_msg_type_t type) {
  if (!chat) return -1;
  for (int i = 0; i < chat->n_msgs; i++) {
    if (chat->msgs[i].msg_type == type)
      return i;
  }
  return -1;
}

/* Remove a range of messages [start, end). Frees all fields. */
void llm_chat_remove_range(llm_chat_t *chat, int start, int end) {
  if (!chat || start < 0 || end > chat->n_msgs || start >= end) return;
  for (int i = start; i < end; i++) {
    chat->total_chars -= (long)chat->msgs[i].content_len;
    llm_msg_free_fields(&chat->msgs[i]);
  }
  int tail = chat->n_msgs - end;
  if (tail > 0)
    memmove(&chat->msgs[start], &chat->msgs[end],
            tail * sizeof(llm_msg_t));
  chat->n_msgs -= (end - start);
}

/* FIX D5: Insert a typed message at a specific position.
 * Grows array if needed, shifts messages from pos..n_msgs-1 forward.
 * BUG 4 FIX: strdup BEFORE memmove — if strdup fails, chat array is untouched. */
int llm_chat_insert_typed(llm_chat_t *chat, int pos,
                          const char *role, const char *content,
                          llm_msg_type_t type) {
  if (!chat || pos < 0 || pos > chat->n_msgs) return -1;
  if (llm_chat_ensure_capacity(chat) != 0) return -1;
  /* BUG 4 FIX: Allocate strings BEFORE shifting array.
     * Previously, memmove ran first, then strdup failure left a
     * corrupted hole at pos with shifted messages at pos+1..n_msgs. */
  char *r = xstrdup(role);
  char *c = xstrdup(content);
  /* Shift existing messages to make room */
  int tail = chat->n_msgs - pos;
  if (tail > 0)
    memmove(&chat->msgs[pos + 1], &chat->msgs[pos],
            tail * sizeof(llm_msg_t));
  llm_msg_t *m = &chat->msgs[pos];
  memset(m, 0, sizeof(*m));
  m->role = r;
  m->content = c;
  m->content_len = strlen(c);
  chat->total_chars += (long)m->content_len;
  m->msg_type = type;
  m->importance = llm_importance_for_type(type);
  chat->n_msgs++;
  return 0;
}

/* Add a tool result message (role: "tool" with tool_call_id) */
int llm_chat_add_tool_result(llm_chat_t *chat, const char *tool_call_id,
                             const char *content) {
  if (llm_chat_ensure_capacity(chat) != 0) return -1;
  llm_msg_t *m = &chat->msgs[chat->n_msgs];
  memset(m, 0, sizeof(*m));
  m->role = xstrdup("tool");
  m->content = xstrdup(content);
  m->tool_call_id = tool_call_id ? xstrdup(tool_call_id) : NULL;
  m->content_len = strlen(m->content);
  chat->total_chars += (long)m->content_len;
  m->msg_type = LLM_MSG_TOOL_RESULT;
  m->importance = llm_importance_for_type(m->msg_type);
  chat->n_msgs++;
  return 0;
}

/* Add an assistant message with tool_calls (for conversation history).
 * Proposal C: Caches the outbound tool_call_id from tool_calls_json at creation
 * time for O(1) partner matching during eviction (eliminates repeated JSON parsing). */
int llm_chat_add_assistant_tool_call(llm_chat_t *chat, const char *content,
                                     const char *tool_calls_json) {
  if (llm_chat_ensure_capacity(chat) != 0) return -1;
  llm_msg_t *m = &chat->msgs[chat->n_msgs];
  memset(m, 0, sizeof(*m));
  m->role = xstrdup("assistant");
  m->content = content ? xstrdup(content) : xstrdup("");
  m->tool_calls_json = tool_calls_json ? xstrdup(tool_calls_json) : NULL;
  /* Proposal C: Extract and cache outbound tool_call_id from JSON */
  if (m->tool_calls_json) {
    cJSON *tc_arr = cJSON_Parse(m->tool_calls_json);
    if (tc_arr && cJSON_IsArray(tc_arr)) {
      cJSON *first = cJSON_GetArrayItem(tc_arr, 0);
      if (first) {
        const char *id_val = json_str(first, "id");
        if (id_val)
          m->tool_call_id_outbound = xstrdup(id_val);
      }
    }
    cJSON_Delete(tc_arr);
  }
  m->content_len = strlen(m->content);
  chat->total_chars += (long)m->content_len;
  m->msg_type = LLM_MSG_GENERIC; // Assistant tool calls are generally generic context
  m->importance = llm_importance_for_type(m->msg_type);
  chat->n_msgs++;
  return 0;
}

/* Replace the content of message at index `idx` with `new_content`.
 * Ownership: new_content is CONSUMED — caller must not use or free it after
 * this call. Updates content_len and total_chars incrementally. */
void llm_chat_replace_content(llm_chat_t *chat, int idx, char *new_content /*consumed*/) {
  if (!chat || idx < 0 || idx >= chat->n_msgs) {
    free(new_content);
    return;
  }
  if (!new_content) return; /* keep old content — preserves non-NULL invariant */
  llm_msg_t *m = &chat->msgs[idx];
  chat->total_chars -= (long)m->content_len;
  free(m->content);
  m->content = new_content;
  m->content_len = strlen(new_content);
  chat->total_chars += (long)m->content_len;
}

/* ── Chat serialization ──────────────────────────────────────── */

/* Serialize entire chat into a human-readable markdown document.
 * Format per message: ### role\n\ncontent\n\n
 * Returns malloc'd string. Caller must free. */
char *llm_chat_serialize(llm_chat_t *chat) {
  if (!chat || chat->n_msgs == 0) return xstrdup("");
  str_t s = str_new(4096);
  for (int i = 0; i < chat->n_msgs; i++) {
    llm_msg_t *m = &chat->msgs[i];
    str_appendf(&s, "### %s\n\n%s\n\n",
                m->role ? m->role : "unknown",
                m->content ? m->content : "");
  }
  return str_steal(&s);
}

/* ── Parse action from assistant response ────────────────────── */

/* Helper: extract a JSON string value for a given key from partial/broken JSON.
 * Finds "key":"value" (with optional whitespace around :) and returns a
 * malloc'd copy of value (with JSON escapes preserved), or NULL. */
static char *extract_json_string_value(const char *text, const char *key) {
  if (!text || !key) return NULL;

  /* Build search pattern: "key" */
  size_t klen = strlen(key);
  char *pattern = xmalloc(klen + 3);
  pattern[0] = '"';
  memcpy(pattern + 1, key, klen);
  pattern[klen + 1] = '"';
  pattern[klen + 2] = '\0';

  const char *kpos = strstr(text, pattern);
  free(pattern);
  if (!kpos) return NULL;

  /* Skip past "key" and optional whitespace/colon */
  const char *p = kpos + klen + 2;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == ':') p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '"') return NULL;
  p++; /* skip opening quote */

  /* Find closing quote (handle escapes -- count consecutive backslashes) */
  const char *val_start = p;
  while (*p) {
    if (*p == '"') {
      /* Count consecutive backslashes preceding this quote */
      int n_bs = 0;
      const char *b = p - 1;
      while (b >= val_start && *b == '\\') {
        n_bs++;
        b--;
      }
      if (n_bs % 2 == 0) break; /* even backslashes = unescaped quote */
    }
    p++;
  }
  if (!*p) return NULL;

  size_t vlen = (size_t)(p - val_start);
  char *val = xmalloc(vlen + 1);
  memcpy(val, val_start, vlen);
  val[vlen] = '\0';
  return val;
}

/* Parse hybrid JSON+XML tool calls.
 *
 * Some Qwen models produce a hybrid format where the response starts as JSON
 * but switches to XML for parameters mid-stream:
 *
 *   {"thought":"...","action":"file_read","<parameter=path>
 *   /some/path
 *   </parameter>
 *   </function>
 *   </tool_call>
 *
 * Or even:
 *   {"thought":"...","action":"grep_search>
 *   <parameter=pattern>
 *   some_pattern
 *   </parameter>
 *
 * This parser extracts the action name from the partial JSON prefix and
 * the parameters from the XML <parameter=KEY>VALUE</parameter> blocks.
 *
 * Returns a cJSON object matching our unified format:
 *   {"action":"func_name", "param1":"value1", ...}
 * or NULL if no hybrid format is detected. */
static cJSON *parse_hybrid_tool_call(const char *text) {
  if (!text) return NULL;

  /* Only trigger if <parameter= is present — our hybrid signature */
  if (!strstr(text, "<parameter=")) return NULL;

  cJSON *result = cJSON_CreateObject();
  if (!result) return NULL;

  /* Extract thought from partial JSON */
  char *thought = extract_json_string_value(text, "thought");
  if (thought) {
    cJSON_AddStringToObject(result, "thought", thought);
    free(thought);
  }

  /* Extract action name — try clean JSON value first */
  char *action = extract_json_string_value(text, "action");
  if (!action) {
    /* Fallback: action name may be glued to XML, e.g. "action":"grep_search>
         * Look for "action":" and grab text up to quote, >, or newline */
    const char *apos = strstr(text, "\"action\"");
    if (!apos) apos = strstr(text, "'action'");
    if (apos) {
      const char *p = apos + 8; /* skip "action" */
      while (*p == ' ' || *p == '\t' || *p == ':')
        p++;
      if (*p == '"') p++;
      const char *astart = p;
      while (*p && *p != '"' && *p != '>' && *p != '\n' &&
             *p != '<' && *p != ',')
        p++;
      size_t alen = (size_t)(p - astart);
      if (alen > 0 && alen < 256) {
        action = xmalloc(alen + 1);
        memcpy(action, astart, alen);
        action[alen] = '\0';
      }
    }
  }

  if (!action || !action[0]) {
    free(action);
    cJSON_Delete(result);
    return NULL;
  }

  cJSON_AddStringToObject(result, "action", action);
  free(action);

  /* Extract parameters from <parameter=KEY>VALUE</parameter> blocks */
  const char *p = text;
  while (p && *p) {
    const char *param_tag = strstr(p, "<parameter=");
    if (!param_tag) break;

    /* Extract parameter name */
    const char *pname_start = param_tag + strlen("<parameter=");
    const char *pname_end = pname_start;
    while (*pname_end && *pname_end != '>' && *pname_end != '\n')
      pname_end++;

    char pname[256];
    size_t pname_len = (size_t)(pname_end - pname_start);
    if (pname_len >= sizeof(pname)) pname_len = sizeof(pname) - 1;
    memcpy(pname, pname_start, pname_len);
    pname[pname_len] = '\0';
    /* Trim trailing whitespace */
    while (pname_len > 0 && (pname[pname_len - 1] == ' ' ||
                             pname[pname_len - 1] == '\t')) {
      pname[--pname_len] = '\0';
    }

    /* Extract value: everything between > and </parameter> */
    const char *val_start = pname_end;
    if (*val_start == '>') val_start++;
    if (*val_start == '\n') val_start++;

    const char *val_end = strstr(val_start, "</parameter>");
    if (!val_end) {
      /* No closing tag — take rest up to </function> or end */
      val_end = strstr(val_start, "</function>");
      if (!val_end) val_end = val_start + strlen(val_start);
    }

    /* Trim trailing whitespace from value */
    while (val_end > val_start && (val_end[-1] == '\n' ||
                                   val_end[-1] == '\r' || val_end[-1] == ' '))
      val_end--;

    size_t val_len = (size_t)(val_end - val_start);
    char *val = xmalloc(val_len + 1);
    memcpy(val, val_start, val_len);
    val[val_len] = '\0';
    cJSON_AddStringToObject(result, pname, val);
    free(val);

    /* Advance past </parameter> */
    const char *close = strstr(val_start, "</parameter>");
    if (close) {
      p = close + strlen("</parameter>");
    } else {
      break;
    }
  }

  nash_log("[llm] parsed hybrid JSON+XML tool call (action=%s)",
           json_str_or(result, "action", "(unknown)"));
  return result;
}

/* Parse Qwen-style XML tool calls.
 *
 * Qwen models are trained with two XML formats for tool calling:
 *
 * Format A (Qwen3 native — JSON inside tags):
 *   <tool_call>{"name":"func","arguments":{"param":"val"}}</tool_call>
 *
 * Format B (Hermes/Qwen3.6 — pure XML):
 *   <tool_call>
 *   <function=func_name>
 *   <parameter=param1>
 *   value1
 *   </parameter>
 *   </function>
 *   </tool_call>
 *
 * Returns a cJSON object matching our unified format:
 *   {"action":"func_name", "param1":"value1", ...}
 * or NULL if no XML tool call is found. */
cJSON *parse_xml_tool_call(const char *text, int *multi_count) {
  if (!text) return NULL;

  /* Find <tool_call> tag */
  const char *tc_start = strstr(text, "<tool_call>");
  if (!tc_start) return NULL;

  /* Capture any text before <tool_call> as the "thought" */
  const char *thought_start = text;
  while (*thought_start == ' ' || *thought_start == '\n' ||
         *thought_start == '\r' || *thought_start == '\t')
    thought_start++;
  size_t thought_len = (thought_start < tc_start) ? (size_t)(tc_start - thought_start) : 0;
  /* Trim trailing whitespace from thought */
  while (thought_len > 0 && (thought_start[thought_len - 1] == ' ' ||
                             thought_start[thought_len - 1] == '\n' ||
                             thought_start[thought_len - 1] == '\r'))
    thought_len--;

  const char *body = tc_start + strlen("<tool_call>");

  /* Find </tool_call> (optional — may be truncated) */
  const char *tc_end = strstr(body, "</tool_call>");
  size_t body_len = tc_end ? (size_t)(tc_end - body) : strlen(body);

  /* Count additional <tool_call> occurrences for multi_count */
  if (multi_count) {
    int count = 1;
    const char *scan = tc_end ? (tc_end + strlen("</tool_call>")) : NULL;
    while (scan) {
      scan = strstr(scan, "<tool_call>");
      if (scan) {
        count++;
        scan += strlen("<tool_call>");
      }
    }
    *multi_count = count;
  }

  /* Make a NUL-terminated copy of the body */
  char *body_copy = xmalloc(body_len + 1);
  memcpy(body_copy, body, body_len);
  body_copy[body_len] = '\0';

  /* ── Format A: JSON inside <tool_call> tags ────────────── */
  const char *brace = strchr(body_copy, '{');
  if (brace) {
    cJSON *inner = cJSON_Parse(brace);
    if (inner) {
      /* Convert {"name":"X","arguments":{...}} → {"action":"X",...} */
      cJSON *args = cJSON_GetObjectItem(inner, "arguments");

      cJSON *result = cJSON_CreateObject();
      /* Add thought if there was text before <tool_call> */
      if (thought_len > 0) {
        char *thought = xmalloc(thought_len + 1);
        memcpy(thought, thought_start, thought_len);
        thought[thought_len] = '\0';
        cJSON_AddStringToObject(result, "thought", thought);
        free(thought);
      }
      cJSON_AddStringToObject(result, "action",
                              json_str_or(inner, "name", ""));

      if (args && cJSON_IsObject(args)) {
        cJSON *child = args->child;
        while (child) {
          cJSON *next = child->next;
          cJSON *copy = cJSON_Duplicate(child, 1);
          if (copy) cJSON_AddItemToObject(result, child->string, copy);
          child = next;
        }
      }

      cJSON_Delete(inner);
      free(body_copy);
      nash_log("[llm] parsed Qwen XML tool call (format A: JSON in tags)");
      return result;
    }
  }

  /* ── Format B: Pure XML <function=NAME><parameter=KEY>VAL</parameter> ── */
  const char *fn_start = strstr(body_copy, "<function=");
  if (!fn_start) {
    free(body_copy);
    return NULL;
  }

  /* Extract function name: <function=NAME> or <function=NAME>\n */
  const char *fn_name_start = fn_start + strlen("<function=");
  const char *fn_name_end = fn_name_start;
  while (*fn_name_end && *fn_name_end != '>' && *fn_name_end != '\n')
    fn_name_end++;
  size_t fn_name_len = (size_t)(fn_name_end - fn_name_start);

  cJSON *result = cJSON_CreateObject();
  /* Add thought if there was text before <tool_call> */
  if (thought_len > 0) {
    char *thought = xmalloc(thought_len + 1);
    memcpy(thought, thought_start, thought_len);
    thought[thought_len] = '\0';
    cJSON_AddStringToObject(result, "thought", thought);
    free(thought);
  }
  char fn_name[256];
  if (fn_name_len >= sizeof(fn_name)) fn_name_len = sizeof(fn_name) - 1;
  memcpy(fn_name, fn_name_start, fn_name_len);
  fn_name[fn_name_len] = '\0';
  /* Trim trailing whitespace */
  while (fn_name_len > 0 && (fn_name[fn_name_len - 1] == ' ' ||
                             fn_name[fn_name_len - 1] == '\t')) {
    fn_name[--fn_name_len] = '\0';
  }
  cJSON_AddStringToObject(result, "action", fn_name);

  /* Extract parameters: <parameter=KEY>\nVALUE\n</parameter> */
  const char *p = fn_name_end;
  while (p && *p) {
    const char *param_tag = strstr(p, "<parameter=");
    if (!param_tag) break;

    /* Extract parameter name */
    const char *pname_start = param_tag + strlen("<parameter=");
    const char *pname_end = pname_start;
    while (*pname_end && *pname_end != '>' && *pname_end != '\n')
      pname_end++;

    char pname[256];
    size_t pname_len = (size_t)(pname_end - pname_start);
    if (pname_len >= sizeof(pname)) pname_len = sizeof(pname) - 1;
    memcpy(pname, pname_start, pname_len);
    pname[pname_len] = '\0';
    /* Trim */
    while (pname_len > 0 && (pname[pname_len - 1] == ' ' ||
                             pname[pname_len - 1] == '\t')) {
      pname[--pname_len] = '\0';
    }

    /* Extract value: everything between > and </parameter> */
    const char *val_start = pname_end;
    if (*val_start == '>') val_start++;
    /* Skip leading newline */
    if (*val_start == '\n') val_start++;

    const char *val_end = strstr(val_start, "</parameter>");
    if (!val_end) {
      /* No closing tag — take rest of body (truncated) */
      val_end = body_copy + body_len;
    }

    /* Trim trailing whitespace from value */
    while (val_end > val_start && (val_end[-1] == '\n' ||
                                   val_end[-1] == '\r' || val_end[-1] == ' '))
      val_end--;

    size_t val_len = (size_t)(val_end - val_start);
    char *val = xmalloc(val_len + 1);
    memcpy(val, val_start, val_len);
    val[val_len] = '\0';
    cJSON_AddStringToObject(result, pname, val);
    free(val);

    /* Advance past </parameter> */
    const char *close = strstr(val_start, "</parameter>");
    if (close) {
      p = close + strlen("</parameter>");
    } else {
      break;
    }
  }

  free(body_copy);
  nash_log("[llm] parsed Qwen XML tool call (format B: pure XML)");
  return result;
}

/* Attempt to repair common JSON errors produced by models:
 * - "key="value"  → "key":"value"  (missing colon)
 * - "key=value"   → "key":"value"  (missing colon and quotes)
 * - XML fragments after JSON start → truncated at first < */
static char *repair_json(const char *src) {
  if (!src) return NULL;
  size_t len = strlen(src);
  /* Allocate extra space for inserted colons + post-loop closing quote + NUL */
  char *buf = xmalloc(len * 2 + 3);

  size_t j = 0;
  int in_string = 0;
  int escape = 0;
  int bare_value = 0; /* FIX BUG#10: track inserted opening quote for bare values */

  for (size_t i = 0; src[i] && len > 0 && j < len * 2 - 1; i++) {
    if (escape) {
      buf[j++] = src[i];
      escape = 0;
      continue;
    }
    if (src[i] == '\\') {
      buf[j++] = src[i];
      escape = 1;
      continue;
    }
    if (src[i] == '"') in_string = !in_string;

    /* FIX BUG#10: Close bare value quote before delimiters */
    if (bare_value && !in_string &&
        (src[i] == ',' || src[i] == '}' || src[i] == ']')) {
      buf[j++] = '"';
      bare_value = 0;
    }

    /* Detect XML fragment: truncate at < outside strings */
    if (src[i] == '<' && !in_string) {
      /* Close any open bare value and JSON structure */
      if (bare_value) buf[j++] = '"';
      buf[j++] = '}';
      break;
    }

    /* Fix: "key="value" → "key":"value" (missing colon after key) */
    if (src[i] == '=' && !in_string) {
      /* Check if this looks like "key"=... or "key=... */
      /* Replace = with : */
      buf[j++] = ':';
      /* If next char is not a quote, add one for the value */
      if (src[i + 1] && src[i + 1] != '"' && src[i + 1] != '{' &&
          src[i + 1] != '[' && src[i + 1] != ' ') {
        buf[j++] = '"';
        bare_value = 1; /* FIX BUG#10: remember to close it */
      }
      continue;
    }

    buf[j++] = src[i];
  }
  if (bare_value) buf[j++] = '"'; /* FIX BUG#10: close trailing bare value */
  buf[j] = '\0';
  return buf;
}

cJSON *llm_parse_action(const char *response, int *multi_count) {
  if (multi_count) *multi_count = 0;
  if (!response) return NULL;

  const char *start = response;

  /* Skip leading whitespace and markdown fences */
  start = skip_whitespace(start);
  if (strncmp(start, "```json", 7) == 0) {
    start += 7;
    while (*start == '\n' || *start == '\r')
      start++;
  } else if (strncmp(start, "```", 3) == 0) {
    start += 3;
    while (*start == '\n' || *start == '\r')
      start++;
  }

  const char *brace = strchr(start, '{');
  if (!brace) {
    /* No JSON found — try Qwen XML tool call format as fallback */
    return parse_xml_tool_call(response, multi_count);
  }

  /* Try strict parse first, tracking where parsing ended */
  const char *parse_end = NULL;
  cJSON *action = cJSON_ParseWithOpts(brace, &parse_end, 0);
  if (!action) {
    /* Strict parse failed — try repairing common JSON errors */
    char *repaired = repair_json(brace);
    if (repaired) {
      action = cJSON_Parse(repaired);
      if (action) {
        nash_log("[llm] repaired malformed JSON response");
      }
      free(repaired);
    }
    /* JSON repair also failed — try hybrid JSON+XML format
         * (Qwen models sometimes start JSON then switch to XML parameters) */
    if (!action) {
      action = parse_hybrid_tool_call(response);
    }
    /* Still failed — try pure Qwen XML tool call format */
    if (!action) {
      action = parse_xml_tool_call(response, multi_count);
    }
    if (multi_count && action && *multi_count < 1) *multi_count = 1;
    return action;
  }

  /* Detect concatenated JSON objects (e.g., gemma4/qwen3.6 emitting
     * multiple tool calls as content: {...}{...}).
     * Count how many additional JSON objects follow the first one.
     * Only the first is returned — callers use multi_count to inject
     * a corrective hint telling the model to issue one call at a time. */
  int count = 1;
  if (parse_end && multi_count) {
    const char *rest = parse_end;
    while (rest && *rest) {
      /* Skip whitespace between concatenated objects */
      while (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r')
        rest++;
      if (*rest != '{') break;
      /* Try parsing the next JSON object */
      const char *next_end = NULL;
      cJSON *next = cJSON_ParseWithOpts(rest, &next_end, 0);
      if (!next) break;
      /* Verify it looks like a tool call (has "action" field) */
      if (cJSON_GetObjectItem(next, "action")) {
        count++;
      }
      cJSON_Delete(next);
      rest = next_end;
    }
    *multi_count = count;
  }

  if (count > 1) {
    nash_log("[llm] detected %d concatenated tool calls in content "
             "(only first executed)",
             count);
  }

  return action;
}

/* ── Fetch context size from /props ──────────────────────────── */

int llm_fetch_context_size(const char *api_base) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/props", api_base);

  str_t response = str_new(4096);
  if (http_get(url, 5L, &response) != 0) {
    str_free(&response);
    return 0;
  }

  cJSON *resp = cJSON_Parse(response.data);
  str_free(&response);
  if (!resp) return 0;

  int n_ctx = 0;
  cJSON *dgs = cJSON_GetObjectItem(resp, "default_generation_settings");
  if (dgs) {
    n_ctx = json_int(dgs, "n_ctx", 0);
  }

  cJSON_Delete(resp);
  return n_ctx;
}

/* ── Fetch model name from /v1/models ──────────────────────── */

char *llm_fetch_model_name(const char *api_base) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/v1/models", api_base);

  str_t response = str_new(4096);
  if (http_get(url, 5L, &response) != 0) {
    str_free(&response);
    return NULL;
  }

  cJSON *resp = cJSON_Parse(response.data);
  str_free(&response);
  if (!resp) return NULL;

  char *model_name = NULL;

  /* Try OpenAI format: data[0].id */
  cJSON *data = cJSON_GetObjectItem(resp, "data");
  if (data && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
    cJSON *m0 = cJSON_GetArrayItem(data, 0);
    const char *id_s = json_str(m0, "id");
    if (id_s)
      model_name = xstrdup(id_s);
  }

  /* Fallback: try models[0].model (Ollama format) */
  if (!model_name) {
    cJSON *models = cJSON_GetObjectItem(resp, "models");
    if (models && cJSON_IsArray(models) && cJSON_GetArraySize(models) > 0) {
      cJSON *m0 = cJSON_GetArrayItem(models, 0);
      const char *mn_s = json_str(m0, "model");
      if (mn_s)
        model_name = xstrdup(mn_s);
    }
  }

  cJSON_Delete(resp);
  return model_name;
}

/* ── Fetch raw /props JSON ──────────────────────────────────── */

char *llm_fetch_props_json(const char *api_base) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/props", api_base);

  str_t response = str_new(8192);
  if (http_get(url, 5L, &response) != 0) {
    str_free(&response);
    return NULL;
  }

  return str_steal(&response);
}


/* Apply chat template via /apply-template endpoint (#7).
 * Sends a minimal [{"role":"user","content":query}] and gets back
 * the formatted prompt string. Returns malloc'd string or NULL. */
char *llm_apply_template(const char *api_base, const char *user_query) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/apply-template", api_base);

  cJSON *req = cJSON_CreateObject();
  cJSON *msgs = cJSON_CreateArray();
  cJSON *msg = cjson_msg("user", user_query);
  if (!req || !msgs || !msg) {
    cJSON_Delete(msg);
    cJSON_Delete(msgs);
    cJSON_Delete(req);
    return NULL;
  }
  cJSON_AddItemToArray(msgs, msg);
  cJSON_AddItemToObject(req, "messages", msgs);

  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  str_t response = str_new(4096);
  if (http_post(url, body, headers, 5L, &response) != 0) {
    curl_slist_free_all(headers);
    free(body);
    str_free(&response);
    return NULL;
  }
  curl_slist_free_all(headers);
  free(body);

  /* Parse response: {"prompt": "..."} */
  cJSON *resp = cJSON_Parse(response.data);
  str_free(&response);
  if (!resp) return NULL;

  const char *prompt = json_str(resp, "prompt");
  char *result = NULL;
  if (prompt)
    result = xstrdup(prompt);
  cJSON_Delete(resp);
  return result;
}

/* ── Belief Entropy probe — MMPO [arXiv:2605.30159] ────────────── */

belief_entropy_result_t llm_belief_entropy_probe(const char *api_base,
                                                 const char *memory_context,
                                                 const char *anchor_question,
                                                 int n_predict, int n_probs,
                                                 float temperature) {
  belief_entropy_result_t result = {0, 0, 0, 0};

  /* Build prompt: memory context + anchor question */
  str_t prompt = str_new(8192);
  if (memory_context && strlen(memory_context) > 0) {
    str_appendf(&prompt,
                "Current memory state:\n%s\n\n", memory_context);
  }
  str_appendf(&prompt,
              "Question: %s\n\nAnswer:", anchor_question);

  /* Build /completion request with logprobs */
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "prompt", prompt.data);
  cJSON_AddNumberToObject(req, "n_predict", n_predict);
  cJSON_AddNumberToObject(req, "n_probs", n_probs);
  cJSON_AddNumberToObject(req, "temperature", temperature);
  cJSON_AddBoolToObject(req, "cache_prompt", 1);
  cJSON_AddBoolToObject(req, "stream", 0);

  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  str_free(&prompt);
  if (!body) return result;

  char url[1024];
  snprintf(url, sizeof(url), "%s/completion", api_base);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  str_t response = str_new(16384);
  if (http_post(url, body, headers, 30L, &response) != 0) {
    curl_slist_free_all(headers);
    free(body);
    str_free(&response);
    return result;
  }
  curl_slist_free_all(headers);
  free(body);

  /* Parse response and extract entropy from completion_probabilities */
  cJSON *resp = cJSON_Parse(response.data);
  str_free(&response);
  if (!resp) return result;

  cJSON *probs_arr = cJSON_GetObjectItem(resp, "completion_probabilities");
  if (!probs_arr || !cJSON_IsArray(probs_arr)) {
    cJSON_Delete(resp);
    return result;
  }

  int n_tokens = cJSON_GetArraySize(probs_arr);
  if (n_tokens < 1) {
    cJSON_Delete(resp);
    return result;
  }

  /* Compute per-token entropy from top logprobs */
  float h_sum = 0;
  for (int i = 0; i < n_tokens; i++) {
    cJSON *tok = cJSON_GetArrayItem(probs_arr, i);
    cJSON *top = cJSON_GetObjectItem(tok, "top_logprobs");
    if (!top || !cJSON_IsArray(top)) continue;

    float h = 0;
    int k = cJSON_GetArraySize(top);
    for (int j = 0; j < k; j++) {
      cJSON *entry = cJSON_GetArrayItem(top, j);
      cJSON *lp = cJSON_GetObjectItem(entry, "logprob");
      if (lp && cJSON_IsNumber(lp)) {
        float logp = (float)lp->valuedouble;
        float p = expf(logp);
        if (p > 0) h -= p * logp; /* H = -Σ p·log(p) */
      }
    }
    h_sum += h;
  }

  result.h_mean = h_sum / n_tokens;
  result.h_total = h_sum;
  result.n_tokens = n_tokens;
  result.ok = 1;

  cJSON_Delete(resp);
  return result;
}
