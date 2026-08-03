#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "banner.h"
#include "config.h"
#include "str.h"
#include "nash_limits.h"
#include "cJSON.h"

/* JSON helpers: use global json_str_or / json_num / json_bool from str.h */

/* Print banner: header art + server props + client overrides */
/* Build banner into str_t — shared implementation for both stdio and TUI.
 * use_ansi: 1 = include ANSI color codes (terminal), 0 = plain text (TUI).
 * session_dir: if non-NULL, appended as "[session: ...]" line. */
char *build_banner_impl(const config_t *cfg, const char *props_json,
                                const char *nash_dir, const char *session_dir,
                                const char *profile_file, int use_ansi) {
    str_t s = str_new(2048);

    /* ASCII art header */
    str_append_cstr(&s, "\n");
    if (use_ansi) {
        str_append_cstr(&s, "  \033[1m\033[38;2;80;255;120m _  _    __   ____  _  _  ____  __    __   \033[0m\n");
        str_append_cstr(&s, "  \033[1m\033[38;2;60;220;100m( \\| |  / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \033[0m\n");
        str_append_cstr(&s, "  \033[1m\033[38;2;40;190;80m ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \033[0m\n");
        str_append_cstr(&s, "  \033[1m\033[38;2;30;160;60m(___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \033[0m\n");
        str_append_cstr(&s, "\n");
        str_append_cstr(&s, "  \033[38;2;70;200;90m--------- * New Agentic Shell * ---------\033[0m\n");
    } else {
        str_append_cstr(&s, "   _   _   __   ____  _  _  ____  __    __   \n");
        str_append_cstr(&s, "  ( \\ | | / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \n");
        str_append_cstr(&s, "   ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \n");
        str_append_cstr(&s, "  (___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \n");
        str_append_cstr(&s, "\n");
        str_append_cstr(&s, "  --------- * New Agentic Shell * ---------\n");
    }
    str_append_cstr(&s, "\n");

    /* 1. Provider / server info */
    const char *ptype = cfg->provider.type;
    int is_api = ptype && (strcmp(ptype, "vertex") == 0 ||
                           strcmp(ptype, "anthropic") == 0 ||
                           strcmp(ptype, "openai") == 0);

    if (is_api) {
        str_appendf(&s, "provider: %s\n", ptype);
        str_appendf(&s, "  model:    %s\n",
                    cfg->provider.model_id ? cfg->provider.model_id : "(not set)");
        if (cfg->provider.project_id)
            str_appendf(&s, "  project:  %s\n", cfg->provider.project_id);
        if (cfg->provider.region)
            str_appendf(&s, "  region:   %s\n", cfg->provider.region);
        if (cfg->provider.context_size > 0)
            str_appendf(&s, "  ctx:      %d tok (%dk)\n",
                        cfg->provider.context_size,
                        cfg->provider.context_size / 1024);
        else
            str_append_cstr(&s, "  ctx:      unknown\n");
        str_append_cstr(&s, "\n");
    } else if (!props_json) {
        str_appendf(&s, "server: %s (props unavailable)\n\n",
                    cfg->provider.api_base ? cfg->provider.api_base : "(none)");
    } else {
        cJSON *props = cJSON_Parse(props_json);
        if (!props) {
            str_appendf(&s, "server: %s (props parse error)\n\n",
                        cfg->provider.api_base ? cfg->provider.api_base : "(none)");
        } else {
            cJSON *gs = cJSON_GetObjectItem(props, "default_generation_settings");
            cJSON *params = gs ? cJSON_GetObjectItem(gs, "params") : NULL;
            cJSON *caps = cJSON_GetObjectItem(props, "chat_template_caps");
            cJSON *mods = cJSON_GetObjectItem(props, "modalities");
            int n_ctx = (int)json_num(gs, "n_ctx", 0);

            str_appendf(&s, "server: %s\n", cfg->provider.api_base ? cfg->provider.api_base : "(none)");
            str_appendf(&s, "  model:    %s\n", json_str_or(props, "model_alias", "(unknown)"));
            str_appendf(&s, "  build:    %s\n", json_str_or(props, "build_info", "?"));
            str_appendf(&s, "  ctx:      %d tok (%dk) | slots: %d\n",
                        n_ctx, n_ctx / 1024, (int)json_num(props, "total_slots", 0));

            if (params) {
                str_appendf(&s, "  defaults: temp=%.1f top_k=%d top_p=%.2f min_p=%.2f",
                            json_num(params, "temperature", 0),
                            (int)json_num(params, "top_k", 0),
                            json_num(params, "top_p", 0),
                            json_num(params, "min_p", 0));
                double rp = json_num(params, "repeat_penalty", 1.0);
                if (rp != 1.0) str_appendf(&s, " rep=%.1f", rp);
                str_append_cstr(&s, "\n");
            }

            str_appendf(&s, "  caps:     tools=%s vision=%s reasoning=%s\n",
                        caps && json_bool(caps, "supports_tools", 0) ? "yes" : "no",
                        mods && json_bool(mods, "vision", 0) ? "yes" : "no",
                        params ? json_str_or(params, "reasoning_format", "none") : "?");
            str_append_cstr(&s, "\n");
            cJSON_Delete(props);
        }
    }

    /* 2. Client config */
    const char *think_str;
    if (cfg->thinking.mode == THINKING_ON)
        think_str = "yes";
    else
        think_str = "no";
    str_appendf(&s, "client: temp=%.1f max_tokens=%d thinking=%s stream=%s\n",
                cfg->temperature, cfg->max_tokens,
                think_str,
                cfg->stream ? "on" : "off");
    str_appendf(&s, "data:   %s\n", nash_dir);
    char cwd_buf[NASH_PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)))
        str_appendf(&s, "cwd:    %s\n", cwd_buf);
    if (profile_file)
        str_appendf(&s, "profile: %s\n", profile_file);
    if (session_dir)
        str_appendf(&s, "\n[session: %s]\n", session_dir);
    if (!session_dir)
        str_append_cstr(&s, "\n");

    return str_steal(&s);
}

/* Print banner to stdout with ANSI colors (CLI mode) */
void print_banner(const config_t *cfg, const char *props_json,
                         const char *nash_dir, const char *profile_file) {
    char *banner = build_banner_impl(cfg, props_json, nash_dir, NULL,
                                     profile_file, 1);
    fputs(banner, stdout);
    free(banner);
}

/* Build banner as a plain-text string for ncurses TUI */
char *build_banner_string(const config_t *cfg, const char *props_json,
                                  const char *nash_dir, const char *session_dir,
                                  const char *profile_file) {
    return build_banner_impl(cfg, props_json, nash_dir, session_dir,
                             profile_file, 0);
}
