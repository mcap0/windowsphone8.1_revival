#include "config.h"
#include "Blocker.h"

#include "Engine.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/URL.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>

// The requests a page makes to advertising and tracking servers are work the phone does for nobody: the script is
// downloaded, compiled and run, it then asks for more, and a page of a newspaper spends more time on them than on
// itself. The list is a table of host names (BlockerList.inc, from engine/tools/blocklist.py: Peter Lowe's list plus
// blocklist/extra.txt); a name covers its subdomains. WebCore asks before every request it makes (subresources,
// frames, redirects, fetch/XHR, beacons, WebSockets, pop-ups: UserContentProvider::processContentRuleListsForLoad,
// the place where Safari's content blockers answer), and a refused one fails as a blocked load, with no network.
//
// Never refused: the page the user opens (a listed host typed or tapped is a choice), and a listed name's requests
// from its own pages (first party: the page's host is that name or under it). ENABLE_CONTENT_EXTENSIONS' own
// compiler is not used: it builds its automata through temporary files mapped copy-on-write and would have to run
// on the phone at the first start; a sorted table needs neither and answers in a few binary searches.

namespace WebCore {
extern bool (*wp81BlockLoadHook)(const URL&, const URL& mainDocumentURL); // UserContentProvider.cpp
}

namespace Lumia {

namespace {

#include "BlockerList.inc"

bool enabled = true;
unsigned stopped;

HashSet<String>& exemptSites()
{
    static NeverDestroyed<HashSet<String>> sites;
    return sites;
}

BitVector& reported()
{
    static NeverDestroyed<BitVector> bits;
    return bits;
}

// As the table is sorted: bytes, a name before the longer ones that start with it.
int compare(StringView host, const char* name)
{
    unsigned i = 0;
    for (; i < host.length(); ++i) {
        unsigned char expected = name[i];
        if (!expected)
            return 1;
        char16_t actual = host[i];
        if (actual != expected)
            return actual < expected ? -1 : 1;
    }
    return name[i] ? -1 : 0;
}

int find(StringView host)
{
    unsigned low = 0;
    unsigned high = blockedHostCount;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        int order = compare(host, blockedHostNames + blockedHostOffsets[middle]);
        if (!order)
            return middle;
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return -1;
}

// The host itself, then each parent down to two labels: "stats.g.doubleclick.net" is found as "doubleclick.net".
int findListed(StringView host)
{
    while (host.endsWith('.'))
        host = host.left(host.length() - 1);
    for (;;) {
        size_t dot = host.find('.');
        if (dot == notFound)
            return -1;
        int index = find(host);
        if (index >= 0)
            return index;
        host = host.substring(dot + 1);
    }
}

bool isSameOrSubdomain(StringView host, StringView name)
{
    if (host.length() == name.length())
        return host == name;
    return host.length() > name.length() && host.endsWith(name) && host[host.length() - name.length() - 1] == '.';
}

bool shouldBlock(const URL& url, const URL& mainDocumentURL)
{
    if (!enabled)
        return false;
    if (!url.protocolIsInHTTPFamily() && !url.protocolIs("ws"_s) && !url.protocolIs("wss"_s))
        return false;
    auto host = url.host();
    int index = findListed(host);
    if (index < 0)
        return false;
    auto name = StringView::fromLatin1(blockedHostNames + blockedHostOffsets[index]);
    auto site = mainDocumentURL.host();
    if (isSameOrSubdomain(site, name))
        return false;
    if (!exemptSites().isEmpty() && exemptSites().contains(site.toString()))
        return false;
    ++stopped;
    // One line per listed name and run: a page asks the same tracker dozens of times.
    if (!reported().quickGet(index)) {
        reported().quickSet(index);
        log("blocco: %s (lista: %s) su %s", host.utf8().data(), name.utf8().data(), site.utf8().data());
    }
    return true;
}

} // namespace

void installBlocker()
{
    reported().ensureSize(blockedHostCount);
    WebCore::wp81BlockLoadHook = shouldBlock;
}

void blockerCommand(const String& argument)
{
    if (argument.isEmpty()) {
        enabled = !enabled;
        log("blocco: %s per le richieste da ora, %u fermate finora, %u nomi in lista", enabled ? "acceso" : "spento", stopped, blockedHostCount);
        return;
    }
    if (argument == "stato"_s) {
        log("blocco: %s, %u richieste fermate, %u nomi in lista, %u siti esclusi", enabled ? "acceso" : "spento", stopped, blockedHostCount, exemptSites().size());
        return;
    }
    auto site = argument.convertToASCIILowercase();
    if (exemptSites().remove(site))
        log("blocco: di nuovo attivo su %s", site.utf8().data());
    else {
        exemptSites().add(site);
        log("blocco: escluso %s (fino alla chiusura dell'app)", site.utf8().data());
    }
}

} // namespace Lumia
