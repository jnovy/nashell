#ifndef SEARXNG_H
#define SEARXNG_H

/* SearXNG container lifecycle management.
 * Handles auto-starting, health checking, and cleanup of
 * podman/docker-based SearXNG search containers. */

/* Ensure SearXNG is running with JSON format support.
 * If a SearXNG instance is running but lacks JSON format, it is killed
 * and restarted with the correct configuration.
 * Returns 0 if SearXNG is available, -1 on failure. */
int ensure_searxng(const char *searxng_url);

/* Perform a search using SearXNG JSON API.
 * Returns a formatted results string (caller frees), or NULL on failure.
 * *out_count receives the number of results. */
char *searxng_search(const char *searxng_url, const char *query,
                     int *out_count, long timeout);

/* Tear down auto-started SearXNG container (called on nash exit). */
void web_search_cleanup(void);

#endif /* SEARXNG_H */
