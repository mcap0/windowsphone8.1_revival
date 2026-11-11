/* Minimal libintl for the Windows Phone 8.1 build of Navit (implemented in wp81_intl.c).
 * Navit's support/glib includes <libgnuintl.h> on Windows; navit_nls.c uses these functions. */
#ifndef NAVIT_WP81_LIBGNUINTL_H
#define NAVIT_WP81_LIBGNUINTL_H

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *msgid);
char *dgettext(const char *domain, const char *msgid);
char *ngettext(const char *msgid, const char *msgid_plural, unsigned long n);
char *dngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n);
char *textdomain(const char *domain);
char *bindtextdomain(const char *domain, const char *dir);
char *bind_textdomain_codeset(const char *domain, const char *codeset);

#ifndef LC_MESSAGES
#define LC_MESSAGES 1729
#endif

#ifdef __cplusplus
}
#endif

#endif
