#include "config.h"
#include "BackForwardList.h"

#include "Engine.h"
#include <WebCore/BackForwardCache.h>
#include <wtf/NeverDestroyed.h>

namespace Lumia {
using namespace WebCore;

namespace {

// Entries kept per tab, as WebKitLegacy's default.
constexpr unsigned capacity = 100;

class HistoryItemClient final : public WebCore::HistoryItemClient {
public:
    static Ref<HistoryItemClient> create() { return adoptRef(*new HistoryItemClient); }

private:
    void historyItemChanged(const HistoryItem&) final { }
    void clearChildren(const HistoryItem&) const final { }
};

} // namespace

WebCore::HistoryItemClient& historyItemClient()
{
    static NeverDestroyed<Ref<HistoryItemClient>> client { HistoryItemClient::create() };
    return client.get();
}

void BackForwardList::addItem(Ref<HistoryItem>&& item)
{
    // A new page drops whatever was ahead of the current one.
    if (m_current != noCurrentItem) {
        while (m_entries.size() > m_current + 1)
            removeEntryAt(m_entries.size() - 1);
    }
    // Over capacity the oldest entry goes.
    if (m_entries.size() == capacity && m_current && m_current != noCurrentItem) {
        removeEntryAt(0);
        --m_current;
    }
    m_entryHash.add(item.ptr());
    m_entries.insert(m_current == noCurrentItem ? 0 : m_current + 1, WTF::move(item));
    m_current = m_current == noCurrentItem ? 0 : m_current + 1;
    notifyIfChanged();
}

void BackForwardList::removeEntryAt(size_t index)
{
    Ref item = m_entries[index];
    m_entries.removeAt(index);
    m_entryHash.remove(item.ptr());
    BackForwardCache::singleton().remove(item);
}

void BackForwardList::goToItem(HistoryItem& item)
{
    for (unsigned index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].ptr() == &item) {
            m_current = index;
            break;
        }
    }
    notifyIfChanged();
}

RefPtr<HistoryItem> BackForwardList::itemAtIndex(int index, FrameIdentifier)
{
    return itemAtOffset(index);
}

RefPtr<HistoryItem> BackForwardList::itemAtOffset(int offset) const
{
    if (m_current == noCurrentItem)
        return nullptr;
    // Range checks without arithmetic on offset, which comes from the page (history.go).
    if (offset < -static_cast<int>(m_current) || offset > static_cast<int>(forwardListCount()))
        return nullptr;
    return m_entries[m_current + offset].ptr();
}

RefPtr<HistoryItem> BackForwardList::currentItem() const
{
    return itemAtOffset(0);
}

unsigned BackForwardList::backListCount() const
{
    return m_current == noCurrentItem ? 0 : m_current;
}

unsigned BackForwardList::forwardListCount() const
{
    return m_current == noCurrentItem ? 0 : m_entries.size() - m_current - 1;
}

bool BackForwardList::containsItem(const HistoryItem& item) const
{
    return m_entryHash.contains(const_cast<HistoryItem*>(&item));
}

void BackForwardList::close()
{
    while (!m_entries.isEmpty())
        removeEntryAt(m_entries.size() - 1);
    m_current = noCurrentItem;
    m_view = nullptr;
}

void BackForwardList::notifyIfChanged()
{
    int bits = (backListCount() ? 1 : 0) | (forwardListCount() ? 2 : 0);
    if (!m_view || bits == std::exchange(m_lastReported, bits))
        return;
    postEvent(*m_view, LW_EVENT_HISTORY, bits);
}

} // namespace Lumia
