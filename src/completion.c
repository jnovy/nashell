/*
 * completion.c -- Tab-completion engine for slash commands.
 *
 * Hierarchical command tree with static subcommands and dynamic providers
 * for playbook names, agent IDs, run IDs, and filesystem paths.
 *
 * Completion flow:
 *   1. Parse input into tokens (command, subcommand, arg)
 *   2. Match first token against top-level commands
 *   3. Match subsequent tokens against subcommands/flags
 *   4. Call dynamic providers when appropriate
 *   5. Compute longest common prefix for ambiguous matches
 */

#include "completion.h"
#include "nash_limits.h"
#include "agents.h"
#include "playbook.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

/* ── Helpers ─────────────────────────────────────────────── */

/* Add a candidate to a dynamically growing array if it matches the prefix. */
static void add_candidate(char ***arr, int *count, int *cap,
                          const char *value, const char *prefix, int prefix_len) {
    if (prefix_len > 0 && strncmp(value, prefix, (size_t)prefix_len) != 0)
        return;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = realloc(*arr, (size_t)*cap * sizeof(char *));
    }
    (*arr)[(*count)++] = strdup(value);
}

/* Compute the longest common prefix of an array of strings. */
static char *longest_common_prefix(char **arr, int count) {
    if (count == 0) return strdup("");
    if (count == 1) return strdup(arr[0]);
    int len = (int)strlen(arr[0]);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < len && arr[i][j] && arr[0][j] == arr[i][j])
            j++;
        len = j;
    }
    return strndup(arr[0], (size_t)len);
}

/* ── Command tree definition ─────────────────────────────── */

/* Dynamic completion provider signature.
 * Fills candidates array with matches for `prefix`.
 * Returns number of candidates added. */
typedef int (*dyn_provider_t)(const char *nash_dir, const char *prefix,
                              int prefix_len, char ***arr, int *count, int *cap);

typedef struct {
    const char     *name;         /* command name (without /) */
    const char    **subcommands;  /* NULL-terminated static subcommand list */
    dyn_provider_t  dyn_sub;      /* dynamic subcommand provider (NULL if none) */
    dyn_provider_t  dyn_arg;      /* dynamic arg provider for specific subcommands */
    const char    **arg_subcmds;  /* subcommands that accept dynamic args (NULL-terminated) */
} cmd_def_t;

/* ── Dynamic providers ───────────────────────────────────── */

static int provide_playbooks(const char *nash_dir, const char *prefix,
                             int prefix_len, char ***arr, int *count, int *cap) {
    if (!nash_dir) return 0;
    int pb_count = 0;
    playbook_t **pbs = playbook_list(nash_dir, &pb_count);
    if (!pbs) return 0;
    int added = 0;
    for (int i = 0; i < pb_count; i++) {
        if (pbs[i]->name) {
            int before = *count;
            add_candidate(arr, count, cap, pbs[i]->name, prefix, prefix_len);
            if (*count > before) added++;
        }
        playbook_free(pbs[i]);
    }
    free(pbs);
    return added;
}

static int provide_agents(const char *nash_dir, const char *prefix,
                          int prefix_len, char ***arr, int *count, int *cap) {
    if (!nash_dir) return 0;
    agent_queue_t *q = agent_scan(nash_dir);
    if (!q) return 0;
    int added = 0;
    for (int i = 0; i < q->n_agents; i++) {
        if (q->agents[i].id) {
            int before = *count;
            add_candidate(arr, count, cap, q->agents[i].id, prefix, prefix_len);
            if (*count > before) added++;
        }
    }
    agent_queue_free(q);
    return added;
}

static int provide_runs(const char *nash_dir, const char *prefix,
                        int prefix_len, char ***arr, int *count, int *cap) {
    if (!nash_dir) return 0;
    char rdir[NASH_PATH_MAX];
    snprintf(rdir, sizeof(rdir), "%s/runs", nash_dir);
    DIR *d = opendir(rdir);
    if (!d) return 0;
    int added = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        /* Strip .jsonl suffix for display */
        char *dot = strstr(ent->d_name, ".jsonl");
        char name[256];
        if (dot) {
            int nlen = (int)(dot - ent->d_name);
            if (nlen >= (int)sizeof(name)) nlen = (int)sizeof(name) - 1;
            memcpy(name, ent->d_name, (size_t)nlen);
            name[nlen] = '\0';
        } else {
            snprintf(name, sizeof(name), "%s", ent->d_name);
        }
        int before = *count;
        add_candidate(arr, count, cap, name, prefix, prefix_len);
        if (*count > before) added++;
    }
    closedir(d);
    return added;
}

static int provide_dirs(const char *nash_dir, const char *prefix,
                        int prefix_len, char ***arr, int *count, int *cap) {
    (void)nash_dir;
    /* Filesystem directory completion.
     * Split prefix into dirname + basename for opendir. */
    if (prefix_len == 0) {
        /* Show current directory entries */
        DIR *d = opendir(".");
        if (!d) return 0;
        int added = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            struct stat st;
            if (stat(ent->d_name, &st) == 0 && S_ISDIR(st.st_mode)) {
                int before = *count;
                add_candidate(arr, count, cap, ent->d_name, "", 0);
                if (*count > before) added++;
            }
        }
        closedir(d);
        return added;
    }

    /* Expand ~ to home directory */
    char expanded[NASH_PATH_MAX];
    const char *path = prefix;
    if (prefix_len > 0 && prefix[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(expanded, sizeof(expanded), "%s%.*s",
                     home, prefix_len - 1, prefix + 1);
            path = expanded;
            prefix_len = (int)strlen(expanded);
        }
    } else {
        /* Copy prefix to expanded for manipulation */
        int plen = prefix_len < (int)sizeof(expanded) - 1 ? prefix_len : (int)sizeof(expanded) - 1;
        memcpy(expanded, prefix, (size_t)plen);
        expanded[plen] = '\0';
        path = expanded;
    }

    /* Find last / to split into dir + base */
    const char *last_slash = strrchr(path, '/');
    char dir_path[NASH_PATH_MAX];
    const char *base;
    int base_len;
    if (last_slash) {
        int dlen = (int)(last_slash - path) + 1;
        if (dlen >= (int)sizeof(dir_path)) dlen = (int)sizeof(dir_path) - 1;
        memcpy(dir_path, path, (size_t)dlen);
        dir_path[dlen] = '\0';
        base = last_slash + 1;
        base_len = prefix_len - (int)(base - path);
        if (path == expanded && prefix[0] == '~') {
            /* Adjust base_len for tilde expansion */
            base_len = (int)strlen(base);
        }
    } else {
        strcpy(dir_path, ".");
        base = path;
        base_len = prefix_len;
    }

    DIR *d = opendir(dir_path);
    if (!d) return 0;
    int added = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (base_len > 0 && strncmp(ent->d_name, base, (size_t)base_len) != 0)
            continue;
        /* Check if it's a directory */
        char full[NASH_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        /* Build the candidate using the original prefix format */
        char cand[NASH_PATH_MAX];
        if (prefix[0] == '~' && last_slash) {
            /* Reconstruct with ~ prefix */
            const char *home = getenv("HOME");
            int home_len = home ? (int)strlen(home) : 0;
            if (home && strncmp(path, home, (size_t)home_len) == 0) {
                snprintf(cand, sizeof(cand), "~%.*s%s",
                         (int)(last_slash - path) - home_len + 1,
                         path + home_len,
                         ent->d_name);
            } else {
                snprintf(cand, sizeof(cand), "%s%s",
                         strcmp(dir_path, ".") == 0 ? "" : dir_path,
                         ent->d_name);
            }
        } else if (last_slash) {
            snprintf(cand, sizeof(cand), "%.*s%s",
                     (int)(last_slash - path) + 1, path, ent->d_name);
        } else {
            snprintf(cand, sizeof(cand), "%s", ent->d_name);
        }
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 16;
            *arr = realloc(*arr, (size_t)*cap * sizeof(char *));
        }
        (*arr)[(*count)++] = strdup(cand);
        added++;
    }
    closedir(d);
    return added;
}

/* ── Static subcommand arrays ────────────────────────────── */

static const char *agent_subs[] = {
    "list", "show", "run", "history", "due", "result", NULL
};
static const char *agent_arg_subs[] = {
    "show", "run", "history", "result", NULL
};
static const char *todo_subs[] = {
    "list", "add", "done", "remove", "rm", "purge", "-w", NULL
};
static const char *play_subs[] = {
    "list", NULL
};
static const char *runs_subs[] = {
    "list", "show", NULL
};
static const char *runs_arg_subs[] = {
    "show", NULL
};
static const char *ms_flags[] = {
    "-q", "-k", "-p", "-r", "-n", "-d", NULL
};

/* ── Command definitions table ───────────────────────────── */

static const cmd_def_t command_defs[] = {
    { "fork",          NULL,      NULL, NULL, NULL },
    { "name",          NULL,      NULL, NULL, NULL },
    { "cwd",           NULL,      NULL, provide_dirs, NULL },
    { "dream",         NULL,      NULL, NULL, NULL },
    { "play",          play_subs, provide_playbooks, NULL, NULL },
    { "runs",          runs_subs, NULL, provide_runs, runs_arg_subs },
    { "memory_search", ms_flags,  NULL, NULL, NULL },
    { "ms",            ms_flags,  NULL, NULL, NULL },
    { "?",             NULL,      NULL, NULL, NULL },
    { "todo",          todo_subs, NULL, NULL, NULL },
    { "agent",         agent_subs, NULL, provide_agents, agent_arg_subs },
    { "continue",      NULL,      NULL, NULL, NULL },
};
static const int n_command_defs = sizeof(command_defs) / sizeof(command_defs[0]);

/* ── Token parsing helpers ───────────────────────────────── */

/* Extract the token at position `pos` in `buf`.
 * Tokens are whitespace-separated.  Token 0 is the command (after /).
 * Returns: start offset (in buf) and length via pointers.
 * Returns -1 if the token doesn't exist. */
static int find_token(const char *buf, int len, int token_idx,
                      int *tok_start, int *tok_len) {
    int i = 0;
    /* Skip the leading / */
    if (i < len && buf[i] == '/') i++;

    int cur_token = 0;
    while (i < len) {
        /* Skip spaces */
        while (i < len && buf[i] == ' ') i++;
        if (i >= len) break;

        /* Start of token */
        int start = i;
        while (i < len && buf[i] != ' ') i++;

        if (cur_token == token_idx) {
            *tok_start = start;
            *tok_len = i - start;
            return 0;
        }
        cur_token++;
    }
    return -1;
}

/* Count the number of complete tokens in buf (after /). */
static int count_tokens(const char *buf, int len) {
    int i = 0;
    if (i < len && buf[i] == '/') i++;
    int count = 0;
    while (i < len) {
        while (i < len && buf[i] == ' ') i++;
        if (i >= len) break;
        while (i < len && buf[i] != ' ') i++;
        count++;
    }
    return count;
}

/* Check if cursor is positioned after a trailing space (i.e., ready for next token). */
static int cursor_after_space(const char *buf, int len, int cursor) {
    (void)len;
    return cursor > 0 && buf[cursor - 1] == ' ';
}

/* Check if a subcommand name is in a NULL-terminated list. */
static int subcmd_in_list(const char *subcmd, const char **list) {
    if (!list) return 0;
    for (int i = 0; list[i]; i++) {
        if (strcmp(subcmd, list[i]) == 0) return 1;
    }
    return 0;
}

/* ── Main completion logic ───────────────────────────────── */

completion_result_t *completion_complete(const char *buf, int len, int cursor,
                                         const char *nash_dir) {
    if (len <= 0 || buf[0] != '/') return NULL;

    int ntokens = count_tokens(buf, cursor);  /* tokens up to cursor */
    int after_space = cursor_after_space(buf, len, cursor);

    /* Determine what we're completing:
     *   - Token 0 (the command name): "/ag|" -> complete command
     *   - Token 1 (subcommand): "/agent li|" -> complete subcommand
     *   - Token 2+ (argument): "/agent run fo|" -> complete argument
     *   - After trailing space: "/agent |" -> show all subcommands
     */

    char **candidates = NULL;
    int ccount = 0, ccap = 0;
    int replace_start = 0, replace_len = 0;

    if (ntokens == 0 || (ntokens == 1 && !after_space)) {
        /* Completing the command name (token 0) */
        int ts, tl;
        const char *prefix = "";
        int prefix_len = 0;
        if (find_token(buf, cursor, 0, &ts, &tl) == 0) {
            prefix = buf + ts;
            prefix_len = cursor - ts;  /* partial token up to cursor */
            if (prefix_len < 0) prefix_len = 0;
            if (prefix_len > tl) prefix_len = tl;
            replace_start = ts;
            replace_len = tl;
        } else {
            /* Just "/" with nothing after */
            replace_start = 1;
            replace_len = 0;
        }

        for (int i = 0; i < n_command_defs; i++) {
            add_candidate(&candidates, &ccount, &ccap,
                          command_defs[i].name, prefix, prefix_len);
        }
    } else if ((ntokens == 1 && after_space) || (ntokens == 2 && !after_space)) {
        /* Completing a subcommand or first argument */
        int ts0, tl0;
        if (find_token(buf, len, 0, &ts0, &tl0) != 0)
            return NULL;

        /* Find the matching command */
        const cmd_def_t *cmd = NULL;
        for (int i = 0; i < n_command_defs; i++) {
            if ((int)strlen(command_defs[i].name) == tl0 &&
                strncmp(command_defs[i].name, buf + ts0, (size_t)tl0) == 0) {
                cmd = &command_defs[i];
                break;
            }
        }
        if (!cmd) return NULL;

        const char *prefix = "";
        int prefix_len = 0;
        if (ntokens == 2 && !after_space) {
            int ts1, tl1;
            if (find_token(buf, cursor, 1, &ts1, &tl1) == 0) {
                prefix = buf + ts1;
                prefix_len = cursor - ts1;
                if (prefix_len < 0) prefix_len = 0;
                if (prefix_len > tl1) prefix_len = tl1;
                replace_start = ts1;
                replace_len = tl1;
            }
        } else {
            replace_start = cursor;
            replace_len = 0;
        }

        /* Add static subcommands */
        if (cmd->subcommands) {
            for (int i = 0; cmd->subcommands[i]; i++) {
                add_candidate(&candidates, &ccount, &ccap,
                              cmd->subcommands[i], prefix, prefix_len);
            }
        }

        /* Add dynamic subcommands (e.g., playbook names for /play) */
        if (cmd->dyn_sub) {
            cmd->dyn_sub(nash_dir, prefix, prefix_len,
                         &candidates, &ccount, &ccap);
        }

        /* For commands with no subcommands but a direct arg provider (e.g., /cwd) */
        if (!cmd->subcommands && cmd->dyn_arg && !cmd->arg_subcmds) {
            cmd->dyn_arg(nash_dir, prefix, prefix_len,
                         &candidates, &ccount, &ccap);
        }
    } else {
        /* Token 2+: completing an argument after a known subcommand */
        int ts0, tl0;
        if (find_token(buf, len, 0, &ts0, &tl0) != 0)
            return NULL;

        const cmd_def_t *cmd = NULL;
        for (int i = 0; i < n_command_defs; i++) {
            if ((int)strlen(command_defs[i].name) == tl0 &&
                strncmp(command_defs[i].name, buf + ts0, (size_t)tl0) == 0) {
                cmd = &command_defs[i];
                break;
            }
        }
        if (!cmd || !cmd->dyn_arg) return NULL;

        /* Check if the subcommand (token 1) accepts dynamic args */
        int ts1, tl1;
        if (find_token(buf, len, 1, &ts1, &tl1) != 0)
            return NULL;
        char subcmd[64];
        int slen = tl1 < (int)sizeof(subcmd) - 1 ? tl1 : (int)sizeof(subcmd) - 1;
        memcpy(subcmd, buf + ts1, (size_t)slen);
        subcmd[slen] = '\0';

        if (cmd->arg_subcmds && !subcmd_in_list(subcmd, cmd->arg_subcmds))
            return NULL;

        /* For /ms and /memory_search, after a subcommand we offer more flags */
        if (cmd->subcommands == ms_flags) {
            const char *prefix = "";
            int prefix_len = 0;
            int last_ts, last_tl;
            if (!after_space && find_token(buf, cursor, ntokens - 1, &last_ts, &last_tl) == 0) {
                prefix = buf + last_ts;
                prefix_len = cursor - last_ts;
                if (prefix_len < 0) prefix_len = 0;
                if (prefix_len > last_tl) prefix_len = last_tl;
                replace_start = last_ts;
                replace_len = last_tl;
            } else {
                replace_start = cursor;
                replace_len = 0;
            }
            for (int i = 0; ms_flags[i]; i++) {
                add_candidate(&candidates, &ccount, &ccap,
                              ms_flags[i], prefix, prefix_len);
            }
        } else {
            /* Dynamic argument completion */
            const char *prefix = "";
            int prefix_len = 0;
            int last_ts, last_tl;
            if (!after_space && find_token(buf, cursor, ntokens - 1, &last_ts, &last_tl) == 0) {
                prefix = buf + last_ts;
                prefix_len = cursor - last_ts;
                if (prefix_len < 0) prefix_len = 0;
                if (prefix_len > last_tl) prefix_len = last_tl;
                replace_start = last_ts;
                replace_len = last_tl;
            } else {
                replace_start = cursor;
                replace_len = 0;
            }
            cmd->dyn_arg(nash_dir, prefix, prefix_len,
                         &candidates, &ccount, &ccap);
        }
    }

    if (ccount == 0) {
        free(candidates);
        return NULL;
    }

    completion_result_t *r = calloc(1, sizeof(*r));
    r->candidates = candidates;
    r->count = ccount;
    r->replace_start = replace_start;
    r->replace_len = replace_len;
    r->common_prefix = longest_common_prefix(candidates, ccount);
    return r;
}

void completion_result_free(completion_result_t *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++)
        free(r->candidates[i]);
    free(r->candidates);
    free(r->common_prefix);
    free(r);
}

/* ── UI integration ──────────────────────────────────────── */

void ui_state_completion_reset(ui_state_t *ui) {
    if (!ui) return;
    if (ui->completion_candidates) {
        for (int i = 0; i < ui->completion_count; i++)
            free(ui->completion_candidates[i]);
        free(ui->completion_candidates);
        ui->completion_candidates = NULL;
    }
    ui->completion_count = 0;
    ui->completion_index = -1;
    ui->completion_replace_start = 0;
    ui->completion_replace_len = 0;
    ui->completion_shown = 0;
}

/* Replace bytes [start, start+old_len) in the input buffer with `new_text`.
 * Updates input_len, cursor_pos. */
static void replace_input(ui_state_t *ui, int start, int old_len,
                           const char *new_text, int new_len) {
    int delta = new_len - old_len;
    /* Ensure capacity */
    while (ui->input_len + delta >= ui->input_cap - 1) {
        ui->input_cap *= 2;
        ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
    }
    /* Shift tail */
    memmove(ui->input_buffer + start + new_len,
            ui->input_buffer + start + old_len,
            (size_t)(ui->input_len - start - old_len + 1));  /* +1 for NUL */
    /* Copy new text */
    memcpy(ui->input_buffer + start, new_text, (size_t)new_len);
    ui->input_len += delta;
    ui->cursor_pos = start + new_len;
    ui->input_buffer[ui->input_len] = '\0';
}

void ui_state_complete_tab(ui_state_t *ui, int direction) {
    if (!ui || ui->input_len <= 0 || ui->input_buffer[0] != '/') return;

    /* If we already have active candidates, cycle through them */
    if (ui->completion_candidates && ui->completion_count > 1) {
        /* Already showed common prefix on first tab.
         * On second+ tab, cycle through individual candidates. */
        if (!ui->completion_shown) {
            /* Second tab: show candidates in status bar */
            str_t display = str_new(256);
            for (int i = 0; i < ui->completion_count && i < 20; i++) {
                if (i > 0) str_appendf(&display, "  ");
                str_appendf(&display, "%s", ui->completion_candidates[i]);
            }
            if (ui->completion_count > 20)
                str_appendf(&display, "  ... (+%d more)",
                            ui->completion_count - 20);
            char *text = str_steal(&display);
            ui_state_set_status(ui, ui->status, text);
            free(text);
            ui->completion_shown = 1;
            ui->completion_index = -1;
            ui->dirty = 1;
            return;
        }

        /* Third+ tab: cycle through candidates */
        ui->completion_index += direction;
        if (ui->completion_index >= ui->completion_count)
            ui->completion_index = 0;
        if (ui->completion_index < 0)
            ui->completion_index = ui->completion_count - 1;

        const char *pick = ui->completion_candidates[ui->completion_index];
        int pick_len = (int)strlen(pick);

        /* Replace the token */
        replace_input(ui, ui->completion_replace_start,
                      /* Current replacement length: recalculate from cursor */
                      ui->cursor_pos - ui->completion_replace_start,
                      pick, pick_len);
        ui->dirty = 1;
        return;
    }

    /* First tab: compute completions */
    completion_result_t *r = completion_complete(
        ui->input_buffer, ui->input_len, ui->cursor_pos, ui->nash_dir);

    if (!r || r->count == 0) {
        completion_result_free(r);
        return;
    }

    if (r->count == 1) {
        /* Single match: replace and add trailing space */
        const char *match = r->candidates[0];
        int mlen = (int)strlen(match);
        char with_space[NASH_PATH_MAX];
        snprintf(with_space, sizeof(with_space), "%s ", match);
        replace_input(ui, r->replace_start, r->replace_len,
                      with_space, mlen + 1);
        ui->dirty = 1;
        completion_result_free(r);
        return;
    }

    /* Multiple matches: insert common prefix */
    int cp_len = (int)strlen(r->common_prefix);
    int prefix_in_buf = ui->cursor_pos - r->replace_start;
    if (prefix_in_buf < 0) prefix_in_buf = 0;

    if (cp_len > prefix_in_buf) {
        /* The common prefix extends beyond what's already typed */
        replace_input(ui, r->replace_start, r->replace_len,
                      r->common_prefix, cp_len);
    }

    /* Save candidates for cycling on next Tab */
    ui_state_completion_reset(ui);  /* clear any stale state */
    ui->completion_candidates = r->candidates;
    ui->completion_count = r->count;
    ui->completion_index = -1;
    ui->completion_replace_start = r->replace_start;
    ui->completion_replace_len = cp_len;  /* now the full common prefix is in the buffer */
    ui->completion_shown = 0;

    /* Show candidates in status bar immediately if common prefix
     * didn't advance (user already typed the full common prefix) */
    if (cp_len <= prefix_in_buf) {
        str_t display = str_new(256);
        for (int i = 0; i < r->count && i < 20; i++) {
            if (i > 0) str_appendf(&display, "  ");
            str_appendf(&display, "%s", r->candidates[i]);
        }
        if (r->count > 20)
            str_appendf(&display, "  ... (+%d more)", r->count - 20);
        char *text = str_steal(&display);
        ui_state_set_status(ui, ui->status, text);
        free(text);
        ui->completion_shown = 1;
    }

    ui->dirty = 1;

    /* Don't free candidates/common_prefix -- ownership transferred to ui state.
     * Only free the result struct itself. */
    free(r->common_prefix);
    free(r);  /* but NOT r->candidates -- now owned by ui */
}
