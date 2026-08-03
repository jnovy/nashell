/* cmd_todo.c — /todo command implementation (extracted from commands.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "commands.h"
#include "commands_internal.h"
#include "str.h"
#include "nash_limits.h"
#include "tui.h"
#include "ui_state_internal.h"
#include "memory.h"
#include "todo_core.h"

/* Check if user is currently viewing todo.md */
static int viewing_todo(ui_state_t *ui) {
    if (!ui->current_filepath) return 0;
    const char *base = strrchr(ui->current_filepath, '/');
    base = base ? base + 1 : ui->current_filepath;
    return strcmp(base, "todo.md") == 0;
}

/* Regenerate the rendered todo.md in session_dir from workspace todo.md.
 * If the user is viewing todo.md, reload it so the change is visible. */
static void cmd_todo_refresh(ui_state_t *ui, const char *ws_todo_path,
                             const char *ws_name) {
    if (!ui->session_dir) return;

    char tpath[NASH_PATH_MAX];
    snprintf(tpath, sizeof(tpath), "%s/todo.md", ui->session_dir);

    /* Build rendered content */
    str_t out = str_new(1024);
    str_append_cstr(&out, "# TODO List");
    if (ws_name) str_appendf(&out, " (%s)", ws_name);
    str_append_cstr(&out, "\n\n");

    FILE *f = fopen(ws_todo_path, "r");
    if (f) {
        int num = 0, open = 0, done_n = 0;
        char linebuf[4096];
        while (fgets(linebuf, sizeof(linebuf), f)) {
            size_t len = strlen(linebuf);
            while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                linebuf[--len] = '\0';
            if (len == 0) continue;
            num++;
            str_appendf(&out, "%d. %s\n", num, linebuf);
            if (strstr(linebuf, "- [ ]")) open++;
            else if (strstr(linebuf, "- [x]")) done_n++;
        }
        fclose(f);
        if (num == 0)
            str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
        else
            str_appendf(&out, "\n**%d open, %d done** — `/todo add|done|remove|purge`\n",
                        open, done_n);
    } else {
        str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
    }

    FILE *tf = fopen(tpath, "w");
    if (tf) { fputs(str_cstr(&out), tf); fclose(tf); }
    str_free(&out);

    pthread_mutex_lock(&ui->mtx);
    if (viewing_todo(ui))
        ui_state_reload_file(ui);
    pthread_mutex_unlock(&ui->mtx);
}

/* Render a single todo.md file into output buffer.
 * Returns number of items found (0 if file missing/empty).
 * Adds *open_out and *done_out counts. */
static int cmd_todo_render_file(str_t *out, const char *path,
                                int *open_out, int *done_out) {
    int num = 0, open = 0, done_n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char linebuf[4096];
    while (fgets(linebuf, sizeof(linebuf), f)) {
        size_t len = strlen(linebuf);
        while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
            linebuf[--len] = '\0';
        if (len == 0) continue;
        num++;
        str_appendf(out, "%d. %s\n", num, linebuf);
        if (strstr(linebuf, "- [ ]")) open++;
        else if (strstr(linebuf, "- [x]")) done_n++;
    }
    fclose(f);
    if (open_out) *open_out += open;
    if (done_out) *done_out += done_n;
    return num;
}

/* List todos from all workspaces (called when no workspace is active).
 * Enumerates {nash_dir}/workspaces/NAME/todo.md and {nash_dir}/todo.md. */
static int cmd_todo_list_all(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;
    str_t out = str_new(2048);
    str_append_cstr(&out, "# TODO List (all workspaces)\n\n");

    int total_open = 0, total_done = 0, any_items = 0;

    /* Global todo */
    {
        char gpath[NASH_PATH_MAX];
        snprintf(gpath, sizeof(gpath), "%s/todo.md", ctx->nash_dir);
        if (access(gpath, F_OK) == 0) {
            str_append_cstr(&out, "## global\n\n");
            int go = 0, gd = 0;
            int n = cmd_todo_render_file(&out, gpath, &go, &gd);
            if (n > 0) {
                str_appendf(&out, "  *%d open, %d done*\n\n", go, gd);
                total_open += go;
                total_done += gd;
                any_items = 1;
            } else {
                str_append_cstr(&out, "*No items.*\n\n");
            }
        }
    }

    /* Enumerate workspaces */
    {
        char ws_dir[NASH_PATH_MAX];
        snprintf(ws_dir, sizeof(ws_dir), "%s/workspaces", ctx->nash_dir);
        DIR *d = opendir(ws_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char tpath[NASH_PATH_MAX];
                if (snprintf(tpath, sizeof(tpath), "%s/%s/todo.md",
                             ws_dir, ent->d_name)
                        >= (int)sizeof(tpath))
                    continue;
                /* Only show workspaces that have a todo.md */
                if (access(tpath, F_OK) != 0) continue;
                int wo = 0, wd = 0;
                str_appendf(&out, "## %s\n\n", ent->d_name);
                int n = cmd_todo_render_file(&out, tpath, &wo, &wd);
                if (n > 0) {
                    str_appendf(&out, "  *%d open, %d done*\n\n", wo, wd);
                    total_open += wo;
                    total_done += wd;
                    any_items = 1;
                } else {
                    str_append_cstr(&out, "*No items.*\n\n");
                }
            }
            closedir(d);
        }
    }

    if (!any_items)
        str_append_cstr(&out, "*No TODO items in any workspace.*\n");
    else
        str_appendf(&out, "---\n**Total: %d open, %d done**\n",
                    total_open, total_done);

    /* Write to session_dir/todo.md and navigate */
    if (ui->session_dir) {
        char tpath[NASH_PATH_MAX];
        snprintf(tpath, sizeof(tpath), "%s/todo.md", ui->session_dir);
        FILE *tf = fopen(tpath, "w");
        if (tf) { fputs(str_cstr(&out), tf); fclose(tf); }
        str_free(&out);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_READY, "TODO list (all workspaces)");
        free(ui->current_filepath);
        ui->current_filepath = NULL;
        char *new_path = xstrdup(tpath);
        ui->current_filepath = new_path;
        ui->scroll_y = 0;
        ui->scroll_x = 0;
        ui->cursor_link = 0;
        ui->search_active = 0;
        ui_state_reload_file(ui);
        pthread_mutex_unlock(&ui->mtx);
    } else {
        str_free(&out);
    }
    tui_render(ui);
    return CMD_CONTINUE;
}

int cmd_todo(command_ctx_t *ctx, const char *args) {
    ui_state_t *ui = ctx->ui;
    char fpath[NASH_PATH_MAX];

    if (todo_resolve_path(ctx->ws, ctx->memory, fpath, sizeof(fpath)) != 0) {
        ui_locked_set_status(ui, STATUS_ERROR,
            "Cannot determine todo.md path (no workspace or memory)");
        return CMD_CONTINUE;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;

    /* /todo add <text> */
    if (strncmp(args, "add ", 4) == 0) {
        const char *text = args + 4;
        while (*text == ' ') text++;
        if (!*text) {
            ui_locked_set_status(ui, STATUS_ERROR, "/todo add <text>");
            return CMD_CONTINUE;
        }
        /* Load existing lines, append new one, save atomically */
        char **lines = NULL;
        int count = todo_load(fpath, &lines);
        if (count < 0) {
            ui_locked_set_status(ui, STATUS_ERROR, "Out of memory");
            return CMD_CONTINUE;
        }
        char new_line[4096];
        snprintf(new_line, sizeof(new_line), "- [ ] %s", text);
        count = todo_add(&lines, count, new_line);
        if (count < 0) {
            todo_free_lines(lines, count);
            ui_locked_set_status(ui, STATUS_ERROR, "Out of memory");
            return CMD_CONTINUE;
        }
        if (todo_save(fpath, lines, count) != 0) {
            /* Try creating parent dir and retry */
            char *slash = strrchr(fpath, '/');
            if (slash) { *slash = '\0'; mkdir(fpath, 0755); *slash = '/'; }
            if (todo_save(fpath, lines, count) != 0) {
                todo_free_lines(lines, count);
                ui_locked_set_status(ui, STATUS_ERROR, "Cannot write todo.md");
                return CMD_CONTINUE;
            }
        }
        todo_free_lines(lines, count);

        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        ui_locked_set_status_fmt(ui, STATUS_READY, "Added: %s", text);
        return CMD_CONTINUE;
    }

    /* /todo done <N> */
    if (strncmp(args, "done ", 5) == 0) {
        const char *num_start = args + 5;
        while (*num_start == ' ') num_start++;
        char *endptr;
        long val = strtol(num_start, &endptr, 10);
        if (*endptr != '\0' || endptr == num_start) {
            ui_locked_set_status(ui, STATUS_ERROR, "/todo done <number>");
            return CMD_CONTINUE;
        }
        int idx = (int)val;
        if (idx < 1) {
            ui_locked_set_status(ui, STATUS_ERROR, "/todo done <number>");
            return CMD_CONTINUE;
        }
        /* Load, flip, save */
        char **lines = NULL;
        int count = todo_load(fpath, &lines);
        if (count < 0) {
            ui_locked_set_status(ui, STATUS_ERROR, "Out of memory");
            return CMD_CONTINUE;
        }
        if (count == 0) {
            todo_free_lines(lines, count);
            ui_locked_set_status(ui, STATUS_ERROR, "No todo.md found");
            return CMD_CONTINUE;
        }
        const char *done_err = NULL;
        if (todo_mark_done(lines, count, idx, &done_err) != 0) {
            todo_free_lines(lines, count);
            ui_locked_set_status(ui, STATUS_ERROR, done_err ? done_err : "Cannot mark done");
            return CMD_CONTINUE;
        }
        todo_save(fpath, lines, count);
        char status[256];
        snprintf(status, sizeof(status), "Done: %s", lines[idx-1]);
        todo_free_lines(lines, count);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        ui_locked_set_status(ui, STATUS_READY, status);
        return CMD_CONTINUE;
    }

    /* /todo remove <N> */
    {
        const char *num_start = NULL;
        if (strncmp(args, "remove ", 7) == 0) num_start = args + 7;
        else if (strncmp(args, "rm ", 3) == 0) num_start = args + 3;
        else if (strcmp(args, "remove") == 0 || strcmp(args, "rm") == 0) {
            ui_locked_set_status(ui, STATUS_ERROR, "/todo remove <number>");
            return CMD_CONTINUE;
        }
        if (num_start) {
            while (*num_start == ' ') num_start++;
            char *endptr;
            long val = strtol(num_start, &endptr, 10);
            if (*endptr != '\0' || endptr == num_start) {
                ui_locked_set_status(ui, STATUS_ERROR, "/todo remove <number>");
                return CMD_CONTINUE;
            }
            int idx = (int)val;
            if (idx < 1) {
            ui_locked_set_status(ui, STATUS_ERROR, "/todo remove <number>");
            return CMD_CONTINUE;
        }
        char **lines = NULL;
        int count = todo_load(fpath, &lines);
        if (count < 0) {
            ui_locked_set_status(ui, STATUS_ERROR, "Out of memory");
            return CMD_CONTINUE;
        }
        if (count == 0) {
            todo_free_lines(lines, count);
            ui_locked_set_status(ui, STATUS_ERROR, "No todo.md found");
            return CMD_CONTINUE;
        }
        char *removed_text = NULL;
        const char *rm_err = NULL;
        int new_count = todo_remove(lines, count, idx, &removed_text, &rm_err);
        if (new_count < 0) {
            todo_free_lines(lines, count);
            ui_locked_set_status(ui, STATUS_ERROR, rm_err ? rm_err : "Cannot remove");
            return CMD_CONTINUE;
        }
        char status[256];
        snprintf(status, sizeof(status), "Removed: %s", removed_text ? removed_text : "");
        free(removed_text);
        todo_save(fpath, lines, new_count);
        todo_free_lines(lines, new_count);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        ui_locked_set_status(ui, STATUS_READY, status);
        return CMD_CONTINUE;
        }
    }

    /* /todo purge */
    if (strcmp(args, "purge") == 0) {
        char **lines = NULL;
        int count = todo_load(fpath, &lines);
        if (count < 0) {
            ui_locked_set_status(ui, STATUS_ERROR, "Out of memory");
            return CMD_CONTINUE;
        }
        if (count == 0) {
            todo_free_lines(lines, count);
            ui_locked_set_status(ui, STATUS_READY, "No todo.md found (nothing to purge)");
            return CMD_CONTINUE;
        }
        int purged = 0;
        int kept = todo_purge(lines, count, &purged);
        todo_save(fpath, lines, kept);
        todo_free_lines(lines, kept);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        ui_locked_set_status_fmt(ui, STATUS_READY,
            "Purged %d completed items, %d remaining", purged, kept);
        return CMD_CONTINUE;
    }

    /* /todo  or  /todo list — display all items
     * /todo -w — show aggregated todos from all workspaces */
    {
        /* Parse -w flag (all-workspaces view) */
        int show_all = (strcmp(args, "-w") == 0 ||
                        strcmp(args, "list -w") == 0);

        /* Check for unknown subcommand first */
        if (!show_all && strcmp(args, "list") != 0 && args[0] != '\0') {
            ui_locked_set_status(ui, STATUS_ERROR,
                "Unknown subcommand. Usage: /todo [list|add <text>|done <N>|remove <N>|purge|-w]");
            return CMD_CONTINUE;
        }

        /* Explicit -w flag → aggregate all workspaces */
        if (show_all)
            return cmd_todo_list_all(ctx);

        str_t out = str_new(1024);
        str_append_cstr(&out, "# TODO List");
        if (ctx->ws && ctx->ws->name)
            str_appendf(&out, " (%s)", ctx->ws->name);
        str_append_cstr(&out, "\n\n");

        FILE *f = fopen(fpath, "r");
        if (!f) {
            str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
        } else {
            int num = 0, open = 0, done_n = 0;
            char linebuf[4096];
            while (fgets(linebuf, sizeof(linebuf), f)) {
                size_t len = strlen(linebuf);
                while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                    linebuf[--len] = '\0';
                if (len == 0) continue;
                num++;
                str_appendf(&out, "%d. %s\n", num, linebuf);
                if (strstr(linebuf, "- [ ]")) open++;
                else if (strstr(linebuf, "- [x]")) done_n++;
            }
            fclose(f);
            if (num == 0) {
                str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
            } else {
                str_appendf(&out, "\n**%d open, %d done** — `/todo add|done|remove|purge`\n",
                            open, done_n);
            }
        }

        /* Write rendered TODO to {session_dir}/todo.md and navigate there.
         * This keeps session.md clean — TODO gets its own file. */
        if (ui->session_dir) {
            char tpath[NASH_PATH_MAX];
            snprintf(tpath, sizeof(tpath), "%s/todo.md", ui->session_dir);
            FILE *tf = fopen(tpath, "w");
            if (tf) {
                fputs(str_cstr(&out), tf);
                fclose(tf);
            }
            str_free(&out);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_READY, "TODO list");
            free(ui->current_filepath);
            ui->current_filepath = NULL;
            char *new_path = xstrdup(tpath);
            ui->current_filepath = new_path;
            ui->scroll_y = 0;
            ui->scroll_x = 0;
            ui->cursor_link = 0;
            ui->search_active = 0;
            ui_state_reload_file(ui);
            pthread_mutex_unlock(&ui->mtx);
        } else {
            str_free(&out);
        }
        tui_render(ui);
        return CMD_CONTINUE;
    }
}
