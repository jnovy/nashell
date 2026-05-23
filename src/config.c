#include "config.h"
#include "toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Helper: read TOML string, return strdup or NULL */
static char *toml_str(toml_table_t *tbl, const char *key) {
    toml_datum_t d = toml_string_in(tbl, key);
    return d.ok ? d.u.s : NULL;  /* d.u.s is already malloc'd by tomlc99 */
}

static int toml_int(toml_table_t *tbl, const char *key, int def) {
    toml_datum_t d = toml_int_in(tbl, key);
    return d.ok ? (int)d.u.i : def;
}

static double toml_dbl(toml_table_t *tbl, const char *key, double def) {
    toml_datum_t d = toml_double_in(tbl, key);
    return d.ok ? d.u.d : def;
}

static int toml_bl(toml_table_t *tbl, const char *key, int def) {
    toml_datum_t d = toml_bool_in(tbl, key);
    return d.ok ? d.u.b : def;
}

void config_set_defaults(config_t *cfg) {
    if (!cfg->api_base)      cfg->api_base = strdup("http://192.168.1.18:8080");
    if (cfg->temperature == 0) cfg->temperature = 0.7f;
    if (cfg->max_tokens == 0)  cfg->max_tokens = 4096;
    if (cfg->shell_timeout == 0)      cfg->shell_timeout = 300;
    if (cfg->shell_max_output == 0)   cfg->shell_max_output = 512000;
    if (cfg->file_max_size == 0)      cfg->file_max_size = 52428800;
    if (cfg->grep_timeout == 0)       cfg->grep_timeout = 60;
    if (cfg->grep_max_matches == 0)   cfg->grep_max_matches = 50;
    if (cfg->web_timeout == 0)        cfg->web_timeout = 30;
    if (cfg->web_max_size == 0)       cfg->web_max_size = 512000;
    if (cfg->llm_max_response == 0)   cfg->llm_max_response = 10485760;
    if (cfg->llm_repeat_threshold == 0) cfg->llm_repeat_threshold = 100;
    if (cfg->max_react_steps == 0)    cfg->max_react_steps = 50;
    cfg->json_mode = 1;  /* always on for now */
    cfg->stream = 1;     /* always on for now */
    if (!cfg->search_engine) cfg->search_engine = strdup("duckduckgo");
    if (!cfg->searxng_url)   cfg->searxng_url = strdup("http://localhost:8888/search");
}

config_t *config_load(const char *path) {
    config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No config file — use defaults */
        config_set_defaults(cfg);
        return cfg;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);

    if (!root) {
        fprintf(stderr, "[config] parse error: %s\n", errbuf);
        config_set_defaults(cfg);
        return cfg;
    }

    /* [server] */
    toml_table_t *server = toml_table_in(root, "server");
    if (server) {
        cfg->api_base = toml_str(server, "api_base");
    }

    /* [client] */
    toml_table_t *client = toml_table_in(root, "client");
    if (client) {
        cfg->temperature = (float)toml_dbl(client, "temperature", 0);
        cfg->max_tokens  = toml_int(client, "max_tokens", 0);
        cfg->json_mode   = toml_bl(client, "json_mode", 1);
        cfg->thinking    = toml_bl(client, "thinking", 0);
        cfg->stream      = toml_bl(client, "stream", 1);
    }

    /* [limits] */
    toml_table_t *limits = toml_table_in(root, "limits");
    if (limits) {
        cfg->shell_timeout      = toml_int(limits, "shell_timeout", 0);
        cfg->shell_max_output   = toml_int(limits, "shell_max_output", 0);
        cfg->file_max_size      = toml_int(limits, "file_max_size", 0);
        cfg->grep_timeout       = toml_int(limits, "grep_timeout", 0);
        cfg->grep_max_matches   = toml_int(limits, "grep_max_matches", 0);
        cfg->web_timeout        = toml_int(limits, "web_timeout", 0);
        cfg->web_max_size       = toml_int(limits, "web_max_size", 0);
        cfg->llm_max_response   = toml_int(limits, "llm_max_response", 0);
        cfg->llm_repeat_threshold = toml_int(limits, "llm_repeat_threshold", 0);
        cfg->scratchpad_max     = toml_int(limits, "scratchpad_max", 0);
        cfg->max_react_steps    = toml_int(limits, "max_react_steps", 0);
    }

    /* [paths] */
    toml_table_t *paths = toml_table_in(root, "paths");
    if (paths) {
        cfg->data_dir = toml_str(paths, "data_dir");
    }

    /* [search] */
    toml_table_t *search = toml_table_in(root, "search");
    if (search) {
        cfg->search_engine = toml_str(search, "engine");
        cfg->searxng_url   = toml_str(search, "searxng_url");
    }

    toml_free(root);
    config_set_defaults(cfg);
    return cfg;
}

void config_free(config_t *cfg) {
    if (!cfg) return;
    free(cfg->api_base);
    free(cfg->data_dir);
    free(cfg->search_engine);
    free(cfg->searxng_url);
    free(cfg);
}

int config_write_default(const char *path) {
    /* Don't overwrite existing config */
    struct stat st;
    if (stat(path, &st) == 0) return 0;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fputs(
        "# Nash configuration file\n"
        "# Edit as needed — all values shown are defaults\n"
        "\n"
        "[server]\n"
        "api_base = \"http://192.168.1.18:8080\"\n"
        "\n"
        "[client]\n"
        "temperature = 0.7\n"
        "max_tokens = 4096\n"
        "json_mode = true\n"
        "thinking = false\n"
        "stream = true\n"
        "\n"
        "[limits]\n"
        "shell_timeout = 300          # max seconds for shell_exec\n"
        "shell_max_output = 512000    # max bytes of shell output (500KB)\n"
        "file_max_size = 52428800     # max file size for file_read (50MB)\n"
        "grep_timeout = 60            # max seconds for grep_search\n"
        "grep_max_matches = 50        # max grep results\n"
        "web_timeout = 30             # max seconds for web_fetch/web_search\n"
        "web_max_size = 512000        # max bytes for web response (500KB)\n"
        "llm_max_response = 10485760  # max bytes from LLM response (10MB)\n"
        "llm_repeat_threshold = 100   # stop after N identical tokens\n"
        "scratchpad_max = 0           # 0 = auto (5%% of context)\n"
        "max_react_steps = 50\n"
        "\n"
        "[paths]\n"
        "data_dir = \"\"                # empty = ~/.nash/\n"
        "\n"
        "[search]\n"
        "engine = \"duckduckgo\"        # \"duckduckgo\" or \"searxng\"\n"
        "searxng_url = \"http://localhost:8888/search\"\n",
        f);

    fclose(f);
    return 0;
}
