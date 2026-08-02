#include "html_extract.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

/* Case-insensitive prefix match. Returns 1 if s starts with prefix (ASCII). */
int html_ci_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s) return 0;  /* FIX BUG#6: don't read past end of s */
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Decode one HTML entity at &...; Returns decoded char count written to out.
 * *advance = bytes consumed from src (including & and ;). */
int html_decode_entity(const char *src, char *out, int *advance) {
    const char *p = src + 1; /* skip '&' */
    const char *semi = NULL;
    for (const char *q = p; q < src + 12 && *q; q++) {
        if (*q == ';') { semi = q; break; }
    }
    if (!semi) { *advance = 1; out[0] = '&'; return 1; }

    int elen = (int)(semi - p);
    *advance = (int)(semi - src) + 1;

    if (elen == 2 && p[0] == 'l' && p[1] == 't') { out[0] = '<'; return 1; }
    if (elen == 2 && p[0] == 'g' && p[1] == 't') { out[0] = '>'; return 1; }
    if (elen == 3 && p[0] == 'a' && p[1] == 'm' && p[2] == 'p') { out[0] = '&'; return 1; }
    if (elen == 4 && p[0] == 'q' && p[1] == 'u' && p[2] == 'o' && p[3] == 't') { out[0] = '"'; return 1; }
    if (elen == 4 && p[0] == 'a' && p[1] == 'p' && p[2] == 'o' && p[3] == 's') { out[0] = '\''; return 1; }
    if (elen == 4 && p[0] == 'n' && p[1] == 'b' && p[2] == 's' && p[3] == 'p') { out[0] = ' '; return 1; }

    /* &#NNN; or &#xHHH; */
    if (p[0] == '#') {
        unsigned long cp = 0;
        if (p[1] == 'x' || p[1] == 'X')
            cp = strtoul(p + 2, NULL, 16);
        else
            cp = strtoul(p + 1, NULL, 10);
        if (cp > 0 && cp < 128) { out[0] = (char)cp; return 1; }
        if (cp >= 128 && cp <= 0x7FF) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            /* Surrogate codepoints are invalid in UTF-8; replace with U+FFFD */
            out[0] = (char)0xEF; out[1] = (char)0xBF; out[2] = (char)0xBD;
            return 3;
        }
        if (cp >= 0x800 && cp <= 0xFFFF) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
            return 3;
        }
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            out[0] = (char)(0xF0 | (cp >> 18));
            out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (char)(0x80 | (cp & 0x3F));
            return 4;
        }
        out[0] = '?'; return 1;
    }

    /* Unknown entity — pass through as-is */
    out[0] = '&'; *advance = 1; return 1;
}

/* Emit a character to the appropriate output buffer, handling whitespace
 * collapsing (outside <pre> blocks) and newline capping. */
static void html_emit_char(str_t *out, str_t *link_text, int in_link,
                           char c, int in_pre, int *prev_space, int *newline_count) {
    int is_ws = (c == ' ' || c == '\t' || c == '\r');
    int is_nl = (c == '\n');

    if (!in_pre && (is_ws || is_nl)) {
        if (is_nl) {
            if (*newline_count < 2) {
                if (in_link)
                    str_append(link_text, "\n", 1);
                else
                    str_append(out, "\n", 1);
                (*newline_count)++;
            }
        } else if (!*prev_space) {
            if (in_link)
                str_append(link_text, " ", 1);
            else
                str_append(out, " ", 1);
        }
        *prev_space = 1;
    } else {
        if (in_link)
            str_append(link_text, &c, 1);
        else
            str_append(out, &c, 1);
        *prev_space = 0;
        if (!is_nl) *newline_count = 0;
    }
}

/* Extract readable text from HTML. Strips tags, preserves <a href> as markdown
 * links [text](url), strips <script>/<style> blocks entirely, decodes entities,
 * collapses whitespace. Returns malloc'd string; caller frees. */
char *html_extract_text(const char *html, size_t len) {
    if (!html || len == 0) return NULL;

    str_t out = str_new(len / 3);  /* text is typically 1/3 to 1/10 of HTML */
    const char *p = html;
    const char *end = html + len;
    int in_skip = 0;       /* inside <script> or <style> block */
    int prev_space = 0;    /* previous char was whitespace (for collapsing) */
    int newline_count = 0; /* consecutive newlines (cap at 2) */
    int in_pre = 0;        /* inside <pre> block (preserve whitespace) */

    /* Link state: when we encounter <a href="...">, we buffer the link text
     * and emit [text](url) when we see </a> */
    char link_url[2048];
    str_t link_text = {0};
    int in_link = 0;

    while (p < end) {
        if (*p == '<') {
            const char *tag_start = p + 1;

            /* Skip HTML comments <!-- ... --> */
            if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
                const char *cend = strstr(p + 4, "-->");
                if (cend) { p = cend + 3; continue; }
                else { p = end; break; }
            }

            /* Find end of tag */
            const char *gt = memchr(p, '>', end - p);
            if (!gt) break;

            /* Check for skip blocks: <script>, <style> */
            if (html_ci_prefix(tag_start, "script") &&
                (tag_start[6] == '>' || tag_start[6] == ' ' || tag_start[6] == '\t')) {
                in_skip = 1;
                p = gt + 1;
                continue;
            }
            if (html_ci_prefix(tag_start, "style") &&
                (tag_start[5] == '>' || tag_start[5] == ' ' || tag_start[5] == '\t')) {
                in_skip = 1;
                p = gt + 1;
                continue;
            }
            if (in_skip) {
                if (html_ci_prefix(tag_start, "/script") || html_ci_prefix(tag_start, "/style")) {
                    in_skip = 0;
                }
                p = gt + 1;
                continue;
            }

            /* <pre> tracking */
            if (html_ci_prefix(tag_start, "pre") &&
                (tag_start[3] == '>' || tag_start[3] == ' '))
                in_pre = 1;
            if (html_ci_prefix(tag_start, "/pre"))
                in_pre = 0;

            /* <a href="..."> — start capturing link */
            if (html_ci_prefix(tag_start, "a ") || html_ci_prefix(tag_start, "a\t")) {
                /* Extract href */
                const char *href = NULL;
                for (const char *q = tag_start; q <= gt - 4; q++) {
                    if (html_ci_prefix(q, "href")) {
                        q += 4;
                        while (q < gt && (*q == ' ' || *q == '=')) q++;
                        if (q < gt && (*q == '"' || *q == '\'')) {
                            char quote = *q++;
                            href = q;
                            while (q < gt && *q != quote) q++;
                            size_t hlen = (size_t)(q - href);
                            if (hlen > 0 && hlen < sizeof(link_url)) {
                                memcpy(link_url, href, hlen);
                                link_url[hlen] = '\0';
                                /* Decode entities in URL (e.g. &amp; → &) */
                                char *rp = link_url, *wp = link_url;
                                while (*rp) {
                                    if (*rp == '&') {
                                        char dec[4]; int adv = 0;
                                        int dl = html_decode_entity(rp, dec, &adv);
                                        for (int di = 0; di < dl; di++) *wp++ = dec[di];
                                        rp += adv;
                                    } else {
                                        *wp++ = *rp++;
                                    }
                                }
                                *wp = '\0';
                                /* Free previous link_text if nested <a> (malformed HTML) */
                                if (in_link) str_free(&link_text);
                                in_link = 1;
                                link_text = str_new(64);
                            }
                        }
                        break;
                    }
                }
                p = gt + 1;
                continue;
            }

            /* </a> — emit markdown link */
            if (html_ci_prefix(tag_start, "/a")) {
                if (in_link) {
                    /* Emit [text](url) */
                    if (link_text.len > 0) {
                        str_append_cstr(&out, "[");
                        str_append(&out, link_text.data, link_text.len);
                        str_append_cstr(&out, "](");
                        str_append_cstr(&out, link_url);
                        str_append_cstr(&out, ")");
                        prev_space = 0;
                        newline_count = 0;
                    }
                    str_free(&link_text);
                    in_link = 0;
                }
                p = gt + 1;
                continue;
            }

            /* Block-level tags → insert newline */
            int is_block = 0;
            const char *btags[] = {"p", "div", "br", "h1", "h2", "h3", "h4",
                                   "h5", "h6", "li", "tr", "dt", "dd",
                                   "blockquote", "section", "article",
                                   "header", "footer", "nav", "figure",
                                   "figcaption", "main", "aside",
                                   "/p", "/div", "/h1", "/h2", "/h3", "/h4",
                                   "/h5", "/h6", "/li", "/tr", "/ul", "/ol",
                                   "/table", "/blockquote", "/section",
                                   "/article", "/header", "/footer",
                                   NULL};
            for (int i = 0; btags[i]; i++) {
                size_t blen = strlen(btags[i]);
                if (html_ci_prefix(tag_start, btags[i]) &&
                    (tag_start[blen] == '>' || tag_start[blen] == ' ' ||
                     tag_start[blen] == '/' || tag_start[blen] == '\t')) {
                    is_block = 1;
                    break;
                }
            }

            /* <br> and <br/> always produce a newline */
            if (html_ci_prefix(tag_start, "br") &&
                (tag_start[2] == '>' || tag_start[2] == ' ' ||
                 tag_start[2] == '/' || tag_start[2] == '\t'))
                is_block = 1;

            if (is_block && out.len > 0 && newline_count < 2) {
                str_append(&out, "\n", 1);
                newline_count++;
                prev_space = 1;
            }

            p = gt + 1;
            continue;
        }

        /* Inside a skip block — ignore all text */
        if (in_skip) { p++; continue; }

        /* Entity decoding */
        if (*p == '&') {
            char decoded[4];
            int advance = 0;
            int dlen = html_decode_entity(p, decoded, &advance);
            for (int i = 0; i < dlen; i++) {
                html_emit_char(&out, &link_text, in_link, decoded[i],
                               in_pre, &prev_space, &newline_count);
            }
            p += advance;
            continue;
        }

        /* Regular text character */
        html_emit_char(&out, &link_text, in_link, *p,
                       in_pre, &prev_space, &newline_count);
        p++;
    }

    /* Clean up any unclosed link */
    if (in_link && link_text.len > 0)
        str_append(&out, link_text.data, link_text.len);
    str_free(&link_text);  /* safe even if str_new was never called ({0} → free(NULL)) */

    if (out.len == 0) { str_free(&out); return NULL; }
    return str_steal(&out);
}
