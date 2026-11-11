// A tab's session history (WebCore's BackForwardClient): the list WebKitLegacy keeps per WebView (mac/History/
// BackForwardList.mm), with the app told when going back or forward becomes possible or not (LW_EVENT_HISTORY).
// Web thread only.
#pragma once

#include <WebCore/BackForwardClient.h>
#include <WebCore/HistoryItem.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>

struct lw_view;

namespace Lumia {

class BackForwardList final : public WebCore::BackForwardClient {
public:
    static Ref<BackForwardList> create(lw_view& view) { return adoptRef(*new BackForwardList(view)); }

    RefPtr<WebCore::HistoryItem> currentItem() const;
    // The item lw_view_go_back / lw_view_go_forward would load (offset -1 or +1), or null.
    RefPtr<WebCore::HistoryItem> itemAtOffset(int offset) const;

private:
    explicit BackForwardList(lw_view& view)
        : m_view(&view)
    {
    }

    void addItem(Ref<WebCore::HistoryItem>&&) final;
    void setChildItem(WebCore::BackForwardFrameItemIdentifier, Ref<WebCore::HistoryItem>&&) final { }
    void goToItem(WebCore::HistoryItem&) final;
    Vector<Ref<WebCore::HistoryItem>> allItems(WebCore::FrameIdentifier) final { return m_entries; }
    RefPtr<WebCore::HistoryItem> itemAtIndex(int, WebCore::FrameIdentifier) final;
    unsigned backListCount() const final;
    unsigned forwardListCount() const final;
    bool containsItem(const WebCore::HistoryItem&) const final;
    void close() final;

    void removeEntryAt(size_t index);
    void notifyIfChanged();

    lw_view* m_view;
    Vector<Ref<WebCore::HistoryItem>> m_entries;
    HashSet<RefPtr<WebCore::HistoryItem>> m_entryHash;
    static constexpr unsigned noCurrentItem = std::numeric_limits<unsigned>::max();
    unsigned m_current { noCurrentItem };
    int m_lastReported { -1 };
};

// HistoryItem's client: WebKit2 forwards every change to the UI process; here the items are the only copy, so
// nothing needs to hear about them (as LegacyHistoryItemClient, minus the Objective-C notification).
WebCore::HistoryItemClient& historyItemClient();

} // namespace Lumia
