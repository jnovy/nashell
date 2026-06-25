#include "searxng.h"
#include "str.h"
#include "cJSON.h"
#include "nash_log.h"
#include "nash_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

/* Track whether we auto-started a SearXNG container so we can tear it down
 * on nash exit.  0 = not started, 1 = podman, 2 = docker.
 * FIX #15: Track which runtime was used so cleanup uses the correct one. */
static int searxng_auto_started = 0;

/* Run a container command (stop, rm, etc.) via fork/exec.
 * FIX BUG#12: Use fork/exec instead of system() which is not signal-safe.
 * FIX: Added 30s timeout to prevent hang when container runtime blocks. */
static int run_container_cmd(const char *runtime, const char *action, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        /* FIX: chdir to /tmp so podman's pasta networking helper doesn't
         * try to write .lock files in nash's session directory, which
         * triggers SELinux AVC denials (pasta_t can't write user_home_t). */
        if (chdir("/tmp") != 0) chdir("/");
        /* Child: redirect all stdio to /dev/null */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp(runtime, runtime, action, name, (char *)NULL);
        _exit(127);
    }
    /* Wait with 30s timeout */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    while (1) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w > 0) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed > 30.0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        struct timespec sl = {0, 100000000};  /* 100ms */
        nanosleep(&sl, NULL);
    }
}

/* Check if a URL is reachable (HTTP GET, expect 2xx). Returns 1 if up. */
static int searxng_is_running(const char *base_url) {
    str_t body = str_new(256);
    int rc = http_get(base_url, 3, &body);
    str_free(&body);
    return (rc == 0);
}

/* Extract host:port from a URL like "http://localhost:8888/search".
 * Returns the port number, or 8888 as default. */
static int searxng_port_from_url(const char *url) {
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    const char *colon = strchr(p, ':');
    if (colon) {
        int port = atoi(colon + 1);
        if (port > 0 && port < 65536) return port;
    }
    return 8888;
}

/* Build the SearXNG base URL (without /search path) from the configured URL.
 * Caller must free the returned string. */
static char *searxng_base_url(const char *url) {
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t len = (size_t)(slash - url);
        char *base = malloc(len + 1);
        memcpy(base, url, len);
        base[len] = '\0';
        return base;
    }
    return strdup(url);
}

/* Ensure the persistent SearXNG config directory exists at ~/.nash/searxng/
 * with a settings.yml that enables JSON format. */
static const char *ensure_searxng_config_dir(void) {
    static char cfg_dir[NASH_PATH_MAX] = {0};
    if (cfg_dir[0]) return cfg_dir;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/.nash/searxng", home);

    mkdir_p(cfg_dir, 0755);

    char settings_path[NASH_PATH_MAX + 16];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.yml", cfg_dir);

    int needs_write = 0;
    FILE *f = fopen(settings_path, "r");
    if (!f) {
        needs_write = 1;
    } else {
        char line[256];
        int has_json = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "- json")) { has_json = 1; break; }
        }
        fclose(f);
        if (!has_json) needs_write = 1;
    }

    if (needs_write) {
        f = fopen(settings_path, "w");
        if (f) {
            fprintf(f,
                "# Nash auto-generated SearXNG settings\n"
                "# This file is bind-mounted into the SearXNG container.\n"
                "# It uses use_default_settings to inherit all defaults\n"
                "# and only overrides what nash needs (JSON API format).\n"
                "\n"
                "use_default_settings: true\n"
                "\n"
                "search:\n"
                "  formats:\n"
                "    - html\n"
                "    - json\n"
                "\n"
                "server:\n"
                "  secret_key: \"nash-searxng-auto-generated-key\"\n"
            );
            fclose(f);
            nash_log("[nash] Created SearXNG settings at %s", settings_path);
        } else {
            nash_log("[nash] Warning: could not write SearXNG settings to %s", settings_path);
        }
    }

    return cfg_dir;
}

/* Start a SearXNG container using a specific runtime. */
static int searxng_start_with_runtime(const char *runtime, int port, const char *cfg_dir,
                                       const char *vol_suffix) {
    char port_map[32], base_url_env[160], vol_mount[640];
    snprintf(port_map, sizeof(port_map), "%d:8080", port);
    snprintf(base_url_env, sizeof(base_url_env),
             "SEARXNG_BASE_URL=http://localhost:%d/", port);
    snprintf(vol_mount, sizeof(vol_mount), "%s:/etc/searxng%s", cfg_dir, vol_suffix);

    run_container_cmd(runtime, "rm", "nash-searxng");

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        /* FIX: chdir to /tmp so podman's pasta networking helper doesn't
         * try to write .lock files in nash's session directory, which
         * triggers SELinux AVC denials (pasta_t can't write user_home_t). */
        if (chdir("/tmp") != 0) chdir("/");
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp(runtime, runtime, "run", "-d", "--name", "nash-searxng",
               "-p", port_map,
               "-e", base_url_env,
               "-v", vol_mount,
               "docker.io/searxng/searxng:latest",
               (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int searxng_start_container(int port) {
    const char *cfg_dir = ensure_searxng_config_dir();

    int rc = searxng_start_with_runtime("podman", port, cfg_dir, ":rw,Z");
    if (rc == 0) {
        searxng_auto_started = 1;
    } else {
        rc = searxng_start_with_runtime("docker", port, cfg_dir, ":rw");
        if (rc == 0) searxng_auto_started = 2;
    }
    if (rc != 0) return -1;

    char health_url[256];
    snprintf(health_url, sizeof(health_url), "http://localhost:%d/", port);
    int ready = 0;
    for (int i = 0; i < 30; i++) {
        sleep(1);
        if (searxng_is_running(health_url)) { ready = 1; break; }
    }
    if (!ready) return -1;

    return 0;
}

/* Check if a running SearXNG instance supports JSON format. */
static int searxng_has_json_format(const char *base_url) {
    char probe_url[512];
    snprintf(probe_url, sizeof(probe_url),
             "%s/search?q=test&format=json", base_url);
    str_t body = str_new(256);
    long http_code = 0;
    int rc = http_get_web(probe_url, 5, &body, &http_code);
    str_free(&body);
    if (rc != 0) return 0;
    if (http_code == 403) return 0;
    return 1;
}

/* Kill any existing SearXNG containers. */
static void searxng_kill_existing(void) {
    const char *runtimes[] = {"podman", "docker"};
    const char *names[] = {"searxng", "nash-searxng"};
    for (int r = 0; r < 2; r++) {
        for (int n = 0; n < 2; n++) {
            run_container_cmd(runtimes[r], "stop", names[n]);
            run_container_cmd(runtimes[r], "rm", names[n]);
        }
    }
}

/* Ensure SearXNG is running with JSON format support. */
int ensure_searxng(const char *searxng_url) {
    char *base = searxng_base_url(searxng_url);

    if (searxng_is_running(base)) {
        if (searxng_has_json_format(base)) {
            free(base);
            return 0;
        }
        nash_log("[nash] SearXNG at %s is running but lacks JSON format support "
                 "— killing and restarting with correct config...", base);
        searxng_kill_existing();
        sleep(1);
    }

    int port = searxng_port_from_url(searxng_url);
    nash_log("[nash] SearXNG not running at %s — starting container on port %d...",
             base, port);
    free(base);

    if (searxng_start_container(port) != 0) {
        nash_log("[nash] Failed to start SearXNG container");
        return -1;
    }
    nash_log("[nash] SearXNG container started successfully");
    return 0;
}

/* Check if all engines in a SearXNG response are unresponsive (timeouts).
 * Returns the number of unresponsive engines, 0 if none. */
static int searxng_count_unresponsive(cJSON *root) {
    cJSON *unresponsive = cJSON_GetObjectItem(root, "unresponsive_engines");
    if (!unresponsive || !cJSON_IsArray(unresponsive)) return 0;
    return cJSON_GetArraySize(unresponsive);
}

/* Log unresponsive engines for diagnostics. */
static void searxng_log_unresponsive(cJSON *root) {
    cJSON *unresponsive = cJSON_GetObjectItem(root, "unresponsive_engines");
    if (!unresponsive || !cJSON_IsArray(unresponsive)) return;
    int n = cJSON_GetArraySize(unresponsive);
    for (int i = 0; i < n; i++) {
        cJSON *pair = cJSON_GetArrayItem(unresponsive, i);
        if (!pair || !cJSON_IsArray(pair) || cJSON_GetArraySize(pair) < 2)
            continue;
        cJSON *engine = cJSON_GetArrayItem(pair, 0);
        cJSON *reason = cJSON_GetArrayItem(pair, 1);
        nash_log("[nash] SearXNG engine '%s': %s",
                 engine && engine->valuestring ? engine->valuestring : "?",
                 reason && reason->valuestring ? reason->valuestring : "?");
    }
}

/* Restart the SearXNG container. Returns 0 on success. */
static int searxng_restart_container(const char *searxng_url) {
    nash_log("[nash] All SearXNG engines unresponsive — restarting container...");

    /* Kill existing containers */
    searxng_kill_existing();
    sleep(1);

    /* Start a fresh one */
    int port = searxng_port_from_url(searxng_url);
    if (searxng_start_container(port) != 0) {
        nash_log("[nash] Failed to restart SearXNG container");
        return -1;
    }
    nash_log("[nash] SearXNG container restarted successfully");
    return 0;
}

/* Internal: perform a single SearXNG query and parse results.
 * Returns formatted text (caller frees) or NULL.
 * Sets *out_count to number of results found.
 * Sets *all_unresponsive to 1 if results are empty AND all engines timed out. */
static char *searxng_search_once(const char *searxng_url, const char *query,
                                  int *out_count, long timeout,
                                  int *all_unresponsive) {
    *all_unresponsive = 0;

    CURL *enc = curl_easy_init();
    if (!enc) return NULL;
    char *encoded_q = curl_easy_escape(enc, query, 0);
    curl_easy_cleanup(enc);

    char url[2048];
    snprintf(url, sizeof(url), "%s?q=%s&format=json&categories=general",
             searxng_url, encoded_q);
    curl_free(encoded_q);

    str_t body = str_new(NASH_INITIAL_BUF);
    long http_code = 0;
    if (http_get_web(url, timeout, &body, &http_code) != 0) {
        str_free(&body);
        return NULL;
    }

    if (http_code == 403) {
        nash_log(
                "[nash] SearXNG returned 403 Forbidden for JSON format. "
                "Check ~/.nash/searxng/settings.yml has 'json' in "
                "search.formats and restart the container.");
        str_free(&body);
        return NULL;
    }

    cJSON *root = cJSON_Parse(body.data);
    str_free(&body);
    if (!root) return NULL;

    cJSON *results_arr = cJSON_GetObjectItem(root, "results");
    if (!results_arr || !cJSON_IsArray(results_arr)) {
        /* Check unresponsive before bailing */
        int n_unresponsive = searxng_count_unresponsive(root);
        if (n_unresponsive > 0) {
            searxng_log_unresponsive(root);
            *all_unresponsive = 1;
        }
        cJSON_Delete(root);
        return NULL;
    }

    str_t results = str_new(4096);
    int count = 0;
    int arr_size = cJSON_GetArraySize(results_arr);

    for (int i = 0; i < arr_size && count < 10; i++) {
        cJSON *item = cJSON_GetArrayItem(results_arr, i);
        if (!item) continue;

        cJSON *title_j   = cJSON_GetObjectItem(item, "title");
        cJSON *url_j     = cJSON_GetObjectItem(item, "url");
        cJSON *content_j = cJSON_GetObjectItem(item, "content");

        const char *title   = (title_j && title_j->valuestring) ? title_j->valuestring : "";
        const char *item_url = (url_j && url_j->valuestring) ? url_j->valuestring : "";
        const char *content = (content_j && content_j->valuestring) ? content_j->valuestring : "";

        if (!item_url[0]) continue;

        count++;
        if (title[0] && content[0]) {
            str_appendf(&results, "%d. [%s](%s)\n   %s\n\n", count, title, item_url, content);
        } else if (title[0]) {
            str_appendf(&results, "%d. [%s](%s)\n\n", count, title, item_url);
        } else {
            str_appendf(&results, "%d. %s\n\n", count, item_url);
        }
    }

    /* Check for degraded engines (always log for diagnostics) */
    int n_unresponsive = searxng_count_unresponsive(root);
    if (n_unresponsive > 0) {
        searxng_log_unresponsive(root);
        if (count == 0)
            *all_unresponsive = 1;
    }

    cJSON_Delete(root);
    *out_count = count;

    if (count == 0) {
        str_free(&results);
        return NULL;
    }

    return str_steal(&results);
}

/* Perform a search using SearXNG JSON API.
 * On any failure (0 results), immediately restart the container to clear
 * all engine suspensions/bans/CAPTCHAs, then retry once.
 * Returns a formatted results string (caller frees), or NULL on failure.
 * *out_count receives the number of results. */
char *searxng_search(const char *searxng_url, const char *query,
                     int *out_count, long timeout) {
    int all_unresponsive = 0;
    char *result = searxng_search_once(searxng_url, query, out_count, timeout,
                                        &all_unresponsive);

    /* If we got results, return them */
    if (result) return result;

    /* Always go nuclear: restart container to reset all engine state */
    nash_log("[nash] web_search: 0 results for '%s'%s "
             "— restarting container...",
             query, all_unresponsive ? " (engines unresponsive)" : "");

    if (searxng_restart_container(searxng_url) == 0) {
        int retry_unresponsive = 0;
        result = searxng_search_once(searxng_url, query, out_count, timeout,
                                      &retry_unresponsive);
        if (result) {
            nash_log("[nash] web_search: retry after container restart "
                     "succeeded (%d results)", *out_count);
            return result;
        }
        nash_log("[nash] web_search: retry after container restart "
                 "still returned 0 results%s",
                 retry_unresponsive ? " (engines still unresponsive)" : "");
    }

    return NULL;
}

/* Tear down auto-started SearXNG container. Called on nash exit. */
void web_search_cleanup(void) {
    if (!searxng_auto_started) return;
    nash_log("[nash] Stopping auto-started SearXNG container...");
    const char *runtime = (searxng_auto_started == 1) ? "podman" : "docker";
    run_container_cmd(runtime, "stop", "nash-searxng");
    run_container_cmd(runtime, "rm", "nash-searxng");
    searxng_auto_started = 0;
}
