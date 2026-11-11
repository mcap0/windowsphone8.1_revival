/* Minimal gettext for Navit on Windows Phone 8.1: reads <dir>/<lang>/LC_MESSAGES/<domain>.mo (GNU .mo format).
 * LANG comes from the environment table (wp81_compat.c), e.g. "it_IT" tries "it_IT" then "it". */
#include "wp81_force.h"
#include <stdio.h>
#include "libgnuintl.h"

#define MAX_DOMAINS 8

struct catalog {
    char *domain;
    char *dir;
    int loaded;
    unsigned char *data;
    size_t size;
    unsigned int count;
    unsigned int orig_off;
    unsigned int trans_off;
    int plural_gt1; /* plural rule is (n > 1) instead of (n != 1) */
};

static struct catalog catalogs[MAX_DOMAINS];
static char *current_domain;

static unsigned int read_u32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

static struct catalog *find_catalog(const char *domain, int create) {
    int i;
    for (i = 0; i < MAX_DOMAINS; i++)
        if (catalogs[i].domain && !strcmp(catalogs[i].domain, domain))
            return &catalogs[i];
    if (!create)
        return NULL;
    for (i = 0; i < MAX_DOMAINS; i++)
        if (!catalogs[i].domain) {
            catalogs[i].domain = _strdup(domain);
            return &catalogs[i];
        }
    return NULL;
}

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 28) {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char *)malloc(len);
    if (buf && fread(buf, 1, len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)len;
    return buf;
}

static const char *entry_string(struct catalog *c, unsigned int table, unsigned int index, unsigned int *len) {
    unsigned int off = table + index * 8;
    unsigned int l, o;
    if (off + 8 > c->size)
        return NULL;
    l = read_u32(c->data + off);
    o = read_u32(c->data + off + 4);
    if (o + l >= c->size)
        return NULL;
    if (len)
        *len = l;
    return (const char *)c->data + o;
}

static void load_catalog(struct catalog *c) {
    char path[MAX_PATH];
    char lang[32];
    const char *env;
    const char *header;
    char *dot;
    c->loaded = 1;
    if (!c->dir)
        return;
    env = getenv("LANG");
    if (!env || !*env)
        env = "it_IT";
    strncpy_s(lang, sizeof(lang), env, _TRUNCATE);
    dot = strchr(lang, '.');
    if (dot)
        *dot = '\0';
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/%s/LC_MESSAGES/%s.mo", c->dir, lang, c->domain);
    c->data = read_file(path, &c->size);
    if (!c->data && strchr(lang, '_')) {
        *strchr(lang, '_') = '\0';
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/%s/LC_MESSAGES/%s.mo", c->dir, lang, c->domain);
        c->data = read_file(path, &c->size);
    }
    if (!c->data)
        return;
    if (read_u32(c->data) != 0x950412de) {
        free(c->data);
        c->data = NULL;
        return;
    }
    c->count = read_u32(c->data + 8);
    c->orig_off = read_u32(c->data + 12);
    c->trans_off = read_u32(c->data + 16);
    /* header = translation of "" (always the first, sorted, entry) */
    header = c->count ? entry_string(c, c->trans_off, 0, NULL) : NULL;
    if (header) {
        const char *rule = strstr(header, "plural=");
        if (rule && (strstr(rule, "n > 1") || strstr(rule, "n>1")))
            c->plural_gt1 = 1;
    }
}

static const char *lookup(struct catalog *c, const char *msgid, unsigned int *trans_len) {
    unsigned int lo = 0, hi;
    if (!c->loaded)
        load_catalog(c);
    if (!c->data)
        return NULL;
    hi = c->count;
    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;
        const char *orig = entry_string(c, c->orig_off, mid, NULL);
        int cmp;
        if (!orig)
            return NULL;
        cmp = strcmp(msgid, orig);
        if (cmp == 0) {
            const char *t = entry_string(c, c->trans_off, mid, trans_len);
            return (t && *t) ? t : NULL;
        }
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

char *dgettext(const char *domain, const char *msgid) {
    struct catalog *c = find_catalog(domain ? domain : (current_domain ? current_domain : "navit"), 0);
    const char *t = c ? lookup(c, msgid, NULL) : NULL;
    return (char *)(t ? t : msgid);
}

char *gettext(const char *msgid) {
    return dgettext(NULL, msgid);
}

char *dngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n) {
    struct catalog *c = find_catalog(domain ? domain : (current_domain ? current_domain : "navit"), 0);
    unsigned int len = 0;
    const char *t = c ? lookup(c, msgid, &len) : NULL;
    int index;
    if (!t)
        return (char *)(n == 1 ? msgid : msgid_plural);
    index = c->plural_gt1 ? (n > 1) : (n != 1);
    if (index) {
        size_t first = strlen(t);
        if (first < len)
            return (char *)(t + first + 1);
        return (char *)msgid_plural;
    }
    return (char *)t;
}

char *ngettext(const char *msgid, const char *msgid_plural, unsigned long n) {
    return dngettext(NULL, msgid, msgid_plural, n);
}

char *textdomain(const char *domain) {
    if (domain) {
        free(current_domain);
        current_domain = _strdup(domain);
    }
    return current_domain;
}

char *bindtextdomain(const char *domain, const char *dir) {
    struct catalog *c = find_catalog(domain, 1);
    if (!c)
        return NULL;
    if (dir) {
        free(c->dir);
        c->dir = _strdup(dir);
        if (c->data) {
            free(c->data);
            c->data = NULL;
        }
        c->loaded = 0;
    }
    return c->dir;
}

char *bind_textdomain_codeset(const char *domain, const char *codeset) {
    (void)domain;
    return (char *)codeset; /* .mo files are UTF-8 already */
}
