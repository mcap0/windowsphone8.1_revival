/* Navit speech plugin "wp81": the text goes to the app (Windows.Media.SpeechSynthesis) through a hook. */
#include <config.h>
#include <string.h>
#include <glib.h>
#include "debug.h"
#include "plugin.h"
#include "item.h"
#include "attr.h"
#include "speech.h"
#include "navit_wp81.h"

void (*navit_wp81_speech_hook)(const char *text);
void (*navit_wp81_keep_screen_on_hook)(void);

struct speech_priv {
    int flags;
};

static int speech_wp81_say(struct speech_priv *this_, const char *text) {
    if (!navit_wp81_speech_hook || !text)
        return 0;
    navit_wp81_speech_hook(text);
    return 1;
}

static void speech_wp81_destroy(struct speech_priv *this_) {
    g_free(this_);
}

static struct speech_methods speech_wp81_meth = {
    speech_wp81_destroy,
    speech_wp81_say,
};

static struct speech_priv *speech_wp81_new(struct speech_methods *meth, struct attr **attrs, struct attr *parent) {
    struct speech_priv *this_ = g_new0(struct speech_priv, 1);
    struct attr *flags;
    *meth = speech_wp81_meth;
    if ((flags = attr_search(attrs, attr_flags)))
        this_->flags = flags->u.num;
    return this_;
}

void module_speech_wp81_init(void) {
    plugin_register_category_speech("wp81", speech_wp81_new);
}
