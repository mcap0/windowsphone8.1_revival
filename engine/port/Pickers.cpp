#include "config.h"
#include "Pickers.h"

#include "Engine.h"
#include <WebCore/Color.h>
#include <WebCore/ColorChooser.h>
#include <WebCore/ColorChooserClient.h>
#include <WebCore/ColorSerialization.h>
#include <WebCore/DateTimeChooser.h>
#include <WebCore/DateTimeChooserClient.h>
#include <WebCore/DateTimeChooserParameters.h>
#include <WebCore/PopupMenu.h>
#include <WebCore/PopupMenuClient.h>
#include <wtf/ASCIICType.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefCounted.h>
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Lumia {
using namespace WebCore;

namespace {

// What waits for lw_picker_reply, by event id: each is taken once, by the answer or by the picker's own end.
using Answer = Function<void(std::optional<String>&&)>;

HashMap<int, Answer>& pending()
{
    static NeverDestroyed<HashMap<int, Answer>> map;
    return map;
}

int show(lw_view& view, lw_picker_kind kind, const String& text, const String& text2, Answer&& answer)
{
    static int lastID;
    int id = ++lastID;
    pending().set(id, WTF::move(answer));
    log("selettore %d: tipo %d", id, kind);
    postEvent(view, LW_EVENT_PICKER, kind, text, text2, id);
    return id;
}

// The page ended the picker itself (the element went away, lost the focus, the page navigated): the app closes it.
void close(lw_view& view, int& id)
{
    if (!id)
        return;
    pending().remove(id);
    postEvent(view, LW_EVENT_PICKER, LW_PICKER_CLOSE, { }, { }, id);
    id = 0;
}

String oneLine(const String& text)
{
    return makeStringByReplacingAll(makeStringByReplacingAll(text, '\n', ' '), '\r', ' ');
}

// <select> shown as a menu. The list goes to the app as one line per item, the first character saying what it is.
class SelectPopup final : public PopupMenu {
public:
    SelectPopup(lw_view& view, PopupMenuClient& client)
        : m_view(view)
        , m_client(client)
    {
    }

    ~SelectPopup() final { close(m_view, m_id); }

private:
    void show(const IntRect&, LocalFrameView&, int selectedIndex) final
    {
        RefPtr client = m_client.get();
        if (!client)
            return;
        close(m_view, m_id);
        StringBuilder items;
        int size = client->listSize();
        for (int i = 0; i < size; ++i) {
            char kind = ' ';
            if (client->itemIsSeparator(i))
                kind = '-';
            else if (client->itemIsLabel(i))
                kind = '#';
            else if (!client->itemIsEnabled(i))
                kind = '!';
            else if (i == selectedIndex)
                kind = '*';
            items.append(kind, oneLine(client->itemText(i)), '\n');
        }
        m_id = Lumia::show(m_view, LW_PICKER_SELECT, items.toString(), { }, [this](std::optional<String>&& value) {
            m_id = 0;
            Ref protectedThis { *this };
            RefPtr client = m_client.get();
            if (!client)
                return;
            client->popupDidHide();
            auto index = value ? parseInteger<int>(*value) : std::nullopt;
            if (!index || *index < 0 || *index >= client->listSize() || !client->itemIsEnabled(*index))
                return;
            client->setTextFromItem(*index);
            client->valueChanged(*index);
        });
    }

    void hide() final
    {
        close(m_view, m_id);
        if (RefPtr client = m_client.get())
            client->popupDidHide();
    }

    void updateFromElement() final { }

    void disconnectClient() final
    {
        close(m_view, m_id);
        m_client = nullptr;
    }

    lw_view& m_view;
    WeakPtr<PopupMenuClient> m_client;
    int m_id { 0 };
};

// The date and time inputs. The value travels as the input's own value string ("2026-11-10", "10:30",
// "2026-11-10T10:30", "2026-11", "2026-W46"); the limits as the numbers WebCore keeps (milliseconds, months for month).
class DatePicker final : public RefCounted<DatePicker>, public DateTimeChooser {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DatePicker);
public:
    DatePicker(lw_view& view, DateTimeChooserClient& client)
        : m_view(view)
        , m_client(&client)
    {
    }

    ~DatePicker() final { close(m_view, m_id); }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    void showChooser(const DateTimeChooserParameters& parameters) final
    {
        // Called again while the picker is up when the value changes under it: the one on screen stays.
        if (m_id || m_ended)
            return;
        auto limits = makeString(parameters.minimum, '\n', parameters.maximum, '\n', parameters.step);
        m_id = Lumia::show(m_view, LW_PICKER_DATE, makeString(parameters.type, '\n', parameters.currentValue), limits,
            [this](std::optional<String>&& value) {
                m_id = 0;
                Ref protectedThis { *this };
                if (value && m_client && !m_ended)
                    m_client->didChooseValue(*value);
                // Choosing a value runs the page's listeners, which may have removed the input (endChooser).
                if (m_client && !m_ended)
                    m_client->didEndChooser();
            });
    }

    // The input closes its picker, also from its destructor: it forgets the chooser only in didEndChooser, which
    // therefore comes afterwards, if the input is still there (it owns this object: alive means the input is).
    void endChooser() final
    {
        close(m_view, m_id);
        if (m_ended)
            return;
        m_ended = true;
        RunLoop::mainSingleton().dispatch([weakThis = WeakPtr<DateTimeChooser> { *this }] {
            RefPtr chooser = weakThis.get();
            if (!chooser)
                return;
            auto& picker = static_cast<DatePicker&>(*chooser);
            picker.m_ended = false;
            if (picker.m_client)
                picker.m_client->didEndChooser();
        });
    }

    lw_view& m_view;
    DateTimeChooserClient* m_client;
    int m_id { 0 };
    bool m_ended { false };
};

// <input type=color>: "#rrggbb" both ways, with the page's suggested colours (its datalist) after the value.
class ColorPicker final : public ColorChooser, public RefCountedAndCanMakeWeakPtr<ColorPicker> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ColorPicker);
public:
    ColorPicker(lw_view& view, ColorChooserClient& client, const Color& color)
        : m_view(view)
        , m_client(client)
    {
        show(color);
    }

    ~ColorPicker() final { close(m_view, m_id); }

    void ref() const final { RefCountedAndCanMakeWeakPtr::ref(); }
    void deref() const final { RefCountedAndCanMakeWeakPtr::deref(); }

private:
    void show(const Color& color)
    {
        RefPtr client = m_client.get();
        if (m_id || !client)
            return;
        StringBuilder suggested;
        for (auto& suggestion : client->suggestedColors())
            suggested.append(serializationForHTML(suggestion), '\n');
        m_id = Lumia::show(m_view, LW_PICKER_COLOR, serializationForHTML(color), suggested.toString(), [this](std::optional<String>&& value) {
            m_id = 0;
            Ref protectedThis { *this };
            RefPtr client = m_client.get();
            if (!client)
                return;
            if (value && value->length() == 7 && (*value)[0] == '#') {
                unsigned channels[3];
                bool valid = true;
                for (unsigned i = 0; i < 3; ++i) {
                    char16_t high = (*value)[1 + 2 * i];
                    char16_t low = (*value)[2 + 2 * i];
                    valid = valid && isASCIIHexDigit(high) && isASCIIHexDigit(low);
                    channels[i] = valid ? toASCIIHexValue(high, low) : 0;
                }
                if (valid)
                    client->didChooseColor(Color { SRGBA<uint8_t> { static_cast<uint8_t>(channels[0]), static_cast<uint8_t>(channels[1]), static_cast<uint8_t>(channels[2]) } });
            }
            if (RefPtr stillThere = m_client.get())
                stillThere->didEndChooser();
        });
    }

    void reattachColorChooser(const Color& color) final { show(color); }

    void endChooser() final
    {
        close(m_view, m_id);
        RunLoop::mainSingleton().dispatch([weakThis = WeakPtr { *this }] {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            if (RefPtr client = protectedThis->m_client.get())
                client->didEndChooser();
        });
    }

    lw_view& m_view;
    WeakPtr<ColorChooserClient> m_client;
    int m_id { 0 };
};

} // namespace

RefPtr<PopupMenu> createSelectPopup(lw_view& view, PopupMenuClient& client)
{
    return adoptRef(*new SelectPopup(view, client));
}

RefPtr<DateTimeChooser> createDateTimeChooser(lw_view& view, DateTimeChooserClient& client)
{
    return adoptRef(*new DatePicker(view, client));
}

RefPtr<ColorChooser> createColorChooser(lw_view& view, ColorChooserClient& client, const Color& color)
{
    return adoptRef(*new ColorPicker(view, client, color));
}

bool hasOpenPicker()
{
    return !pending().isEmpty();
}

} // namespace Lumia

void lw_picker_reply(lw_view*, int id, const wchar_t* value)
{
    std::optional<String> answer;
    if (value)
        answer = String(std::span<const char16_t> { reinterpret_cast<const char16_t*>(value), wcslen(value) }).isolatedCopy();
    Lumia::postToWebThread([id, answer = WTF::move(answer)]() mutable {
        if (auto function = Lumia::pending().take(id))
            function(WTF::move(answer));
    }, true);
}
