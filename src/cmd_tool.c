/*
 * cmd_tool.c -- /tool slash-command handlers.
 * Runtime tool enable/disable with profile save/load.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#include "commands.h"
#include "commands_internal.h"
#include "tool_plugin.h"
#include "str.h"
#include "nash_limits.h"
#include "tui.h"
#include "ui_state_internal.h"

/* ── Protected tools (cannot be disabled) ──────────────────── */

static int is_protected_tool(const char *name) {
  return (strcmp(name, "done") == 0 ||
          strcmp(name, "user_ask") == 0);
}

/* ── Runtime blocked list management ──────────────────────── */

/* Check if a tool name is in the runtime-blocked list.
 * The runtime blocked list is stored in tool_ctx_t.tool_filter.blocked/n_blocked.
 * We own this memory (allocated by us), unlike config-based filters. */
static int is_runtime_blocked(const tool_filter_t *tf, const char *name) {
  if (!tf->blocked) return 0;
  for (int i = 0; i < tf->n_blocked; i++) {
    if (strcmp(tf->blocked[i], name) == 0)
      return 1;
  }
  return 0;
}

/* Copy-on-write: the initial tool_filter_t.blocked may point into config
 * memory (set by build_profile_tool_filter in main.c).  Before any mutation,
 * we must deep-copy it to heap memory so realloc/free are safe.
 * Ownership is tracked per-filter via tf->blocked_owned (not a global). */

static void ensure_filter_owned(tool_filter_t *tf) {
  if (tf->blocked_owned) return;
  if (tf->blocked && tf->n_blocked > 0) {
    const char **copy = xmalloc((size_t)tf->n_blocked * sizeof(char *));
    for (int i = 0; i < tf->n_blocked; i++)
      copy[i] = xstrdup(tf->blocked[i]);
    tf->blocked = copy;
  } else {
    tf->blocked = NULL;
    tf->n_blocked = 0;
  }
  tf->blocked_owned = 1;
}

/* Add a tool name to the runtime blocked list. */
static void runtime_block_add(tool_filter_t *tf, const char *name) {
  if (is_runtime_blocked(tf, name)) return;
  ensure_filter_owned(tf);
  if (safe_realloc((void **)&tf->blocked,
                   (size_t)(tf->n_blocked + 1) * sizeof(char *))) return;
  tf->blocked[tf->n_blocked] = xstrdup(name);
  tf->n_blocked++;
}

/* Remove a tool name from the runtime blocked list. */
static void runtime_block_remove(tool_filter_t *tf, const char *name) {
  if (!tf->blocked) return;
  ensure_filter_owned(tf);
  for (int i = 0; i < tf->n_blocked; i++) {
    if (strcmp(tf->blocked[i], name) == 0) {
      free((void *)tf->blocked[i]);
      /* Shift remaining entries */
      for (int j = i; j < tf->n_blocked - 1; j++)
        tf->blocked[j] = tf->blocked[j + 1];
      tf->n_blocked--;
      if (tf->n_blocked == 0) {
        free((void *)tf->blocked);
        tf->blocked = NULL;
      }
      return;
    }
  }
}

/* Free all runtime blocked entries (for reset). */
static void runtime_block_clear(tool_filter_t *tf) {
  if (!tf->blocked_owned) {
    /* Not owned — just clear the pointers without freeing */
    tf->blocked = NULL;
    tf->n_blocked = 0;
    tf->blocked_owned = 1; /* now we own (empty) */
    return;
  }
  if (!tf->blocked) return;
  for (int i = 0; i < tf->n_blocked; i++)
    free((void *)tf->blocked[i]);
  free((void *)tf->blocked);
  tf->blocked = NULL;
  tf->n_blocked = 0;
}

/* ── Apply default-off blocks ─────────────────────────────── */

/* Block all tools with TOOL_FLAG_DEFAULT_OFF that are not already blocked.
 * Called at startup and on /tool reset. */
void tool_apply_default_blocks(tool_filter_t *tf) {
  for (int i = 0; i < tool_plugin_count(); i++) {
    const tool_plugin_t *tp = tool_plugin_get(i);
    if (!tp) continue;
    if ((tp->flags & TOOL_FLAG_DEFAULT_OFF) && !is_runtime_blocked(tf, tp->name))
      runtime_block_add(tf, tp->name);
  }
}

/* ── Find tool by name ────────────────────────────────────── */

/* Returns 0 if found, -1 if not found. */
static int find_tool_index(const char *name) {
  return tool_plugin_find(name) ? 0 : -1;
}

/* ── Profile directory helpers ────────────────────────────── */

static void tool_profiles_dir(const char *nash_dir, char *buf, size_t sz) {
  snprintf(buf, sz, "%s/tool_profiles", nash_dir);
}

static int ensure_profiles_dir(const char *nash_dir) {
  char dir[NASH_PATH_MAX];
  tool_profiles_dir(nash_dir, dir, sizeof(dir));
  return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

/* ── /tool list ────────────────────────────────────────────── */

static int cmd_tool_list(command_ctx_t *ctx) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  str_t display = str_new(2048);
  str_append_cstr(&display, "# Tools\n\n");
  str_append_cstr(&display,
                  "| # | Tool | Status |\n"
                  "|---|------|--------|\n");

  int n_on = 0, n_off = 0;
  for (int i = 0; i < tool_plugin_count(); i++) {
    const tool_plugin_t *tp = tool_plugin_get(i);
    if (!tp) continue;
    const char *name = tp->name;
    int blocked = is_runtime_blocked(tf, name);
    int prot = is_protected_tool(name);

    const char *status;
    if (blocked && (tp->flags & TOOL_FLAG_DEFAULT_OFF))
      status = "OFF (default)";
    else if (blocked)
      status = "OFF";
    else if (prot)
      status = "ON (protected)";
    else
      status = "ON";

    str_appendf(&display, "| %d | `%s` | %s |\n",
                i + 1, name, status);

    if (blocked)
      n_off++;
    else
      n_on++;
  }

  str_appendf(&display,
              "\n**%d enabled**, %d disabled\n\n"
              "Commands: `/tool show NAME`, `/tool on NAME`, `/tool off NAME`, "
              "`/tool reset`, `/tool save PROFILE`, `/tool load PROFILE`\n",
              n_on, n_off);

  char *banner = str_steal(&display);
  pthread_mutex_lock(&ui->mtx);
  ui_state_push_content(ui, "tools", banner);
  ui_state_set_status(ui, STATUS_READY, "Tool list");
  pthread_mutex_unlock(&ui->mtx);
  free(banner);
  tui_render(ui);
  return CMD_CONTINUE;
}

/* ── /tool show NAME ──────────────────────────────────────── */

static int cmd_tool_show(command_ctx_t *ctx, const char *name) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  if (!name || !name[0]) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool show: specify a tool name");
    return CMD_CONTINUE;
  }

  const tool_plugin_t *tp = tool_plugin_find(name);
  if (!tp) {
    ui_locked_set_status_fmt(ui, STATUS_ERROR,
                             "/tool show: unknown tool '%s'", name);
    return CMD_CONTINUE;
  }

  int blocked = is_runtime_blocked(tf, tp->name);
  int prot = is_protected_tool(tp->name);

  const char *status;
  if (blocked && (tp->flags & TOOL_FLAG_DEFAULT_OFF))
    status = "OFF (default)";
  else if (blocked)
    status = "OFF";
  else if (prot)
    status = "ON (protected)";
  else
    status = "ON";

  str_t display = str_new(2048);
  str_appendf(&display, "# %s\n\n", tp->name);
  str_appendf(&display, "**Status:** %s", status);
  if (tp->version)
    str_appendf(&display, "  |  **Version:** %s", tp->version);
  if (tp->group)
    str_appendf(&display, "  |  **Group:** %s", tp->group);
  str_append_cstr(&display, "\n\n");

  /* Description */
  if (tp->description) {
    str_append_cstr(&display, "## Description\n\n");
    str_append_cstr(&display, tp->description);
    str_append_cstr(&display, "\n\n");
  }

  /* Parameters */
  if (tp->params && tp->params[0].name) {
    str_append_cstr(&display, "## Parameters\n\n");
    str_append_cstr(&display,
                    "| Name | Type | Required | Description |\n"
                    "|------|------|----------|-------------|\n");

    for (const tool_param_t *p = tp->params; p->name; p++) {
      const char *type = p->type ? p->type : "string";
      if (p->items_type)
        str_appendf(&display, "| `%s` | %s<%s> | %s | %s",
                    p->name, type, p->items_type,
                    p->required ? "yes" : "no",
                    p->description ? p->description : "");
      else
        str_appendf(&display, "| `%s` | %s | %s | %s",
                    p->name, type,
                    p->required ? "yes" : "no",
                    p->description ? p->description : "");

      /* Enum values */
      if (p->enum_values) {
        str_append_cstr(&display, " Values: ");
        for (const char **ev = p->enum_values; *ev; ev++) {
          if (ev != p->enum_values)
            str_append_cstr(&display, ", ");
          str_appendf(&display, "`%s`", *ev);
        }
        str_append_cstr(&display, ".");
      }
      str_append_cstr(&display, " |\n");
    }
    str_append_cstr(&display, "\n");
  } else {
    str_append_cstr(&display, "*No parameters.*\n\n");
  }

  char *banner = str_steal(&display);
  pthread_mutex_lock(&ui->mtx);
  ui_state_push_content(ui, "tool_show", banner);
  ui_state_set_status(ui, STATUS_READY, tp->name);
  pthread_mutex_unlock(&ui->mtx);
  free(banner);
  tui_render(ui);
  return CMD_CONTINUE;
}

/* ── /tool on NAME ────────────────────────────────────────── */

static int cmd_tool_on(command_ctx_t *ctx, const char *name) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  if (!name || !name[0]) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool on: specify a tool name");
    return CMD_CONTINUE;
  }

  if (find_tool_index(name) < 0) {
    ui_locked_set_status_fmt(ui, STATUS_ERROR,
                             "/tool on: unknown tool '%s'", name);
    return CMD_CONTINUE;
  }

  if (!is_runtime_blocked(tf, name)) {
    ui_locked_set_status_fmt(ui, STATUS_READY,
                             "%s is already enabled", name);
    return CMD_CONTINUE;
  }

  runtime_block_remove(tf, name);
  ui_locked_set_status_fmt(ui, STATUS_READY,
                           "%s enabled (%d/%d tools active)",
                           name, tool_plugin_count() - tf->n_blocked, tool_plugin_count());
  return CMD_CONTINUE;
}

/* ── /tool off NAME ───────────────────────────────────────── */

static int cmd_tool_off(command_ctx_t *ctx, const char *name) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  if (!name || !name[0]) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool off: specify a tool name");
    return CMD_CONTINUE;
  }

  if (find_tool_index(name) < 0) {
    ui_locked_set_status_fmt(ui, STATUS_ERROR,
                             "/tool off: unknown tool '%s'", name);
    return CMD_CONTINUE;
  }

  if (is_protected_tool(name)) {
    ui_locked_set_status_fmt(ui, STATUS_ERROR,
                             "%s cannot be disabled (protected)", name);
    return CMD_CONTINUE;
  }

  if (is_runtime_blocked(tf, name)) {
    ui_locked_set_status_fmt(ui, STATUS_READY,
                             "%s is already disabled", name);
    return CMD_CONTINUE;
  }

  runtime_block_add(tf, name);
  ui_locked_set_status_fmt(ui, STATUS_READY,
                           "%s disabled (%d/%d tools active)",
                           name, tool_plugin_count() - tf->n_blocked, tool_plugin_count());
  return CMD_CONTINUE;
}

/* ── /tool reset ──────────────────────────────────────────── */

static int cmd_tool_reset(command_ctx_t *ctx) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  runtime_block_clear(tf);
  tool_apply_default_blocks(tf);

  ui_locked_set_status_fmt(ui, STATUS_READY,
                           "Tools reset to defaults (%d/%d active)",
                           tool_plugin_count() - tf->n_blocked, tool_plugin_count());
  return CMD_CONTINUE;
}

/* ── /tool save PROFILE ───────────────────────────────────── */

static int cmd_tool_save(command_ctx_t *ctx, const char *profile) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  if (!profile || !profile[0]) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool save: specify a profile name");
    return CMD_CONTINUE;
  }

  /* Validate profile name: alphanumeric + dash + underscore only */
  for (const char *p = profile; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
      ui_locked_set_status(ui, STATUS_ERROR,
                           "/tool save: profile name must be alphanumeric (a-z, 0-9, -, _)");
      return CMD_CONTINUE;
    }
  }

  if (!ensure_profiles_dir(ctx->nash_dir)) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool save: cannot create profiles directory");
    return CMD_CONTINUE;
  }

  char path[NASH_PATH_MAX];
  char dir[NASH_PATH_MAX];
  tool_profiles_dir(ctx->nash_dir, dir, sizeof(dir));
  if (snprintf(path, sizeof(path), "%s/%s", dir, profile) >= (int)sizeof(path)) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool save: path too long");
    return CMD_CONTINUE;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    ui_locked_set_status_fmt(ui, STATUS_ERROR, "/tool save: %s", strerror(errno));
    return CMD_CONTINUE;
  }

  /* Write one disabled tool name per line */
  int n_disabled = 0;
  for (int i = 0; i < tf->n_blocked; i++) {
    fprintf(f, "%s\n", tf->blocked[i]);
    n_disabled++;
  }
  fclose(f);

  ui_locked_set_status_fmt(ui, STATUS_READY,
                           "Profile '%s' saved (%d tool%s disabled)",
                           profile, n_disabled, n_disabled == 1 ? "" : "s");
  return CMD_CONTINUE;
}

/* ── /tool load PROFILE ───────────────────────────────────── */

static int cmd_tool_load(command_ctx_t *ctx, const char *profile) {
  ui_state_t *ui = ctx->ui;
  tool_filter_t *tf = &ctx->tools->tool_filter;

  if (!profile || !profile[0]) {
    /* List available profiles */
    char dir[NASH_PATH_MAX];
    tool_profiles_dir(ctx->nash_dir, dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) {
      ui_locked_set_status(ui, STATUS_ERROR, "No saved profiles");
      return CMD_CONTINUE;
    }

    str_t display = str_new(512);
    str_append_cstr(&display, "# Saved Tool Profiles\n\n");
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
      if (ent->d_name[0] == '.') continue;
      str_appendf(&display, "- `%s`\n", ent->d_name);
      count++;
    }
    closedir(d);

    if (count == 0)
      str_append_cstr(&display, "*No saved profiles.*\n");
    else
      str_appendf(&display, "\n**%d profile%s** -- use `/tool load NAME` to apply\n",
                  count, count == 1 ? "" : "s");

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "tool_profiles", banner);
    ui_state_set_status(ui, STATUS_READY, "Tool profiles");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);
    return CMD_CONTINUE;
  }

  /* Validate profile name: alphanumeric + dash + underscore only
     * (mirrors cmd_tool_save validation, prevents path traversal) */
  for (const char *p = profile; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
      ui_locked_set_status(ui, STATUS_ERROR,
                           "/tool load: profile name must be alphanumeric (a-z, 0-9, -, _)");
      return CMD_CONTINUE;
    }
  }

  char dir[NASH_PATH_MAX];
  tool_profiles_dir(ctx->nash_dir, dir, sizeof(dir));
  char path[NASH_PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/%s", dir, profile) >= (int)sizeof(path)) {
    ui_locked_set_status(ui, STATUS_ERROR, "/tool load: path too long");
    return CMD_CONTINUE;
  }

  FILE *f = fopen(path, "r");
  if (!f) {
    ui_locked_set_status_fmt(ui, STATUS_ERROR,
                             "/tool load: profile '%s' not found", profile);
    return CMD_CONTINUE;
  }

  /* Clear current runtime blocked list and rebuild from profile */
  runtime_block_clear(tf);

  char line[256];
  int loaded = 0;
  while (fgets(line, sizeof(line), f)) {
    /* Strip trailing newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0) continue;

    /* Skip unknown tools silently, skip protected tools */
    if (find_tool_index(line) < 0) continue;
    if (is_protected_tool(line)) continue;

    runtime_block_add(tf, line);
    loaded++;
  }
  fclose(f);

  ui_locked_set_status_fmt(ui, STATUS_READY,
                           "Profile '%s' loaded (%d tool%s disabled, %d active)",
                           profile, loaded, loaded == 1 ? "" : "s",
                           tool_plugin_count() - tf->n_blocked);
  return CMD_CONTINUE;
}

/* ── /tool NAME (toggle) ──────────────────────────────────── */

static int cmd_tool_toggle(command_ctx_t *ctx, const char *name) {
  tool_filter_t *tf = &ctx->tools->tool_filter;

  if (is_runtime_blocked(tf, name))
    return cmd_tool_on(ctx, name);
  else
    return cmd_tool_off(ctx, name);
}

/* ── /tool dispatch ───────────────────────────────────────── */

int cmd_tool(command_ctx_t *ctx, const char *args) {
  /* Skip leading whitespace */
  while (args && *args == ' ')
    args++;

  /* No args or "list" -> show tool list */
  if (!args || !args[0] || strcmp(args, "list") == 0)
    return cmd_tool_list(ctx);

  if (strcmp(args, "reset") == 0)
    return cmd_tool_reset(ctx);

  if (strncmp(args, "on ", 3) == 0) {
    const char *name = args + 3;
    while (*name == ' ')
      name++;
    return cmd_tool_on(ctx, name);
  }

  if (strncmp(args, "off ", 4) == 0) {
    const char *name = args + 4;
    while (*name == ' ')
      name++;
    return cmd_tool_off(ctx, name);
  }

  if (strncmp(args, "show ", 5) == 0) {
    const char *name = args + 5;
    while (*name == ' ')
      name++;
    return cmd_tool_show(ctx, name);
  }

  if (strncmp(args, "save ", 5) == 0) {
    const char *profile = args + 5;
    while (*profile == ' ')
      profile++;
    return cmd_tool_save(ctx, profile);
  }

  if (strcmp(args, "load") == 0)
    return cmd_tool_load(ctx, "");

  if (strncmp(args, "load ", 5) == 0) {
    const char *profile = args + 5;
    while (*profile == ' ')
      profile++;
    return cmd_tool_load(ctx, profile);
  }

  /* If args matches a tool name, toggle it */
  if (find_tool_index(args) >= 0)
    return cmd_tool_toggle(ctx, args);

  /* Unknown subcommand */
  ui_state_t *ui = ctx->ui;
  ui_locked_set_status_fmt(ui, STATUS_ERROR,
                           "/tool: unknown subcommand '%s' -- try list, show, on, off, reset, save, load",
                           args);
  return CMD_CONTINUE;
}
