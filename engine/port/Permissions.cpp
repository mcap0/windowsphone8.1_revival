#include "config.h"
#include "Permissions.h"

#include "Engine.h"
#include <WebCore/Document.h>
#include <WebCore/Geolocation.h>
#include <WebCore/GeolocationClient.h>
#include <WebCore/GeolocationController.h>
#include <WebCore/GeolocationError.h>
#include <WebCore/GeolocationPositionData.h>
#include <WebCore/Notification.h>
#include <WebCore/NotificationClient.h>
#include <WebCore/NotificationData.h>
#include <WebCore/NotificationResources.h>
#include <WebCore/Page.h>
#include <WebCore/ScriptExecutionContext.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/ShareData.h>
#include <cmath>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/Vector.h>
#include <wtf/WallTime.h>
#include <wtf/text/MakeString.h>

namespace Lumia {
using namespace WebCore;

namespace {

// ---- the question ----

// One question on the app's screen and everything that waits for its answer (a page that asks twice, two frames of
// one site). The answer is kept, yes or no, for that site and that kind until the app ends: a site refused once does
// not ask at every page. (On disk, and revocable, with the settings page.)
using Decision = Function<void(bool)>;

struct Question {
    lw_view* view { nullptr };
    String key;
    Vector<Decision> waiting;
};

HashMap<int, Question>& questions()
{
    static NeverDestroyed<HashMap<int, Question>> map;
    return map;
}

HashMap<String, bool>& decisions()
{
    static NeverDestroyed<HashMap<String, bool>> map;
    return map;
}

// A private tab's answers are its own (they neither come from the normal tabs nor stay after it).
String keyFor(lw_view* view, lw_permission_kind kind, const String& origin)
{
    return makeString(view && isViewPrivate(*view) ? "p "_s : ""_s, static_cast<int>(kind), ' ', origin);
}

std::optional<bool> decided(lw_view* view, lw_permission_kind kind, const String& origin)
{
    auto found = decisions().find(keyFor(view, kind, origin));
    if (found == decisions().end())
        return std::nullopt;
    return found->value;
}

void ask(lw_view* view, lw_permission_kind kind, const String& origin, Decision&& decision)
{
    if (auto known = decided(view, kind, origin)) {
        decision(*known);
        return;
    }
    // A tab nobody sees cannot ask: no for now, and the page may ask again when it is in view.
    if (!view || !isViewActive(*view)) {
        log("permesso %d: scheda nascosta, %s senza risposta", static_cast<int>(kind), origin.utf8().data());
        decision(false);
        return;
    }
    auto key = keyFor(view, kind, origin);
    for (auto& question : questions().values()) {
        if (question.key == key) {
            question.waiting.append(WTF::move(decision));
            return;
        }
    }
    static int lastID;
    int id = ++lastID;
    Question question { view, key, { } };
    question.waiting.append(WTF::move(decision));
    questions().set(id, WTF::move(question));
    log("permesso %d: %s chiede (tipo %d)", id, origin.utf8().data(), static_cast<int>(kind));
    postEvent(*view, LW_EVENT_PERMISSION, kind, origin, { }, id);
}

// allowed nullopt: the question went away with its tab; refused, and not remembered.
void answer(int id, std::optional<bool> allowed)
{
    auto question = questions().take(id);
    log("permesso %d: %s, %u in attesa", id, !allowed ? "chiuso" : *allowed ? "si" : "no", static_cast<unsigned>(question.waiting.size()));
    if (allowed)
        decisions().set(question.key, *allowed);
    for (auto& decision : question.waiting)
        decision(allowed.value_or(false));
}

} // namespace

void forgetPermissions(bool privateOnly)
{
    decisions().removeIf([privateOnly](auto& entry) {
        return entry.key.startsWith("p "_s) == privateOnly;
    });
}

namespace {

// ---- geolocation ----

// The phone has one position source, in the app (Windows.Devices.Geolocation): it runs while any page watches
// (LW_EVENT_GEOLOCATION 1, or 2 when a page wants the GPS's accuracy; 0 stops it) and every watching page gets each
// position it reports.
class GeolocationClientWP81;

Vector<GeolocationClientWP81*>& geolocationClients()
{
    static NeverDestroyed<Vector<GeolocationClientWP81*>> clients;
    return clients;
}

std::optional<GeolocationPositionData>& lastKnownPosition()
{
    static NeverDestroyed<std::optional<GeolocationPositionData>> position;
    return position;
}

void deliverPosition(const GeolocationPositionData&);

// The phone's source reports when the phone moves. A page that asks for a fresh position while another request keeps
// the source running would wait for the next movement (WebCore starts nothing new, and a cached position is too old
// for the default maximumAge of 0): so while the source runs and is silent, its last position is said again every few
// seconds with the time of now. It is still where the phone is, by the source's own word.
struct PositionSource {
    int accuracy { 0 };
    bool hasPosition { false };
    MonotonicTime lastDelivery;
    std::unique_ptr<RunLoop::Timer> repeat;
};

PositionSource& positionSource()
{
    static NeverDestroyed<PositionSource> source;
    return source;
}

void repeatPosition()
{
    auto& source = positionSource();
    auto& last = lastKnownPosition();
    if (!source.accuracy || !source.hasPosition || !last || MonotonicTime::now() - source.lastDelivery < 4_s)
        return;
    auto position = *last;
    position.timestamp = WallTime::now().secondsSinceEpoch().seconds();
    deliverPosition(position);
}

class GeolocationClientWP81 final : public GeolocationClient, public RefCounted<GeolocationClientWP81> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(GeolocationClientWP81);
public:
    static Ref<GeolocationClientWP81> create(lw_view& view, Page& page) { return adoptRef(*new GeolocationClientWP81(view, page)); }
    ~GeolocationClientWP81() { geolocationClients().removeFirst(this); }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    lw_view* view() const { return m_view; }
    Page* page() const { return m_page; }
    bool isUpdating() const { return m_updating; }
    bool wantsHighAccuracy() const { return m_highAccuracy; }

    void viewWillClose()
    {
        stopUpdating();
        m_asking.clear();
        m_view = nullptr;
    }

private:
    GeolocationClientWP81(lw_view& view, Page& page)
        : m_view(&view)
        , m_page(&page)
    {
        geolocationClients().append(this);
    }

    void geolocationDestroyed() final
    {
        stopUpdating();
        m_asking.clear();
        m_page = nullptr;
    }

    void startUpdating(const String&, bool needsHighAccuracy) final
    {
        m_updating = true;
        m_highAccuracy = needsHighAccuracy;
        updateSource(m_view);
    }

    void stopUpdating() final
    {
        if (!m_updating)
            return;
        m_updating = false;
        updateSource(m_view);
    }

    void setEnableHighAccuracy(bool enabled) final
    {
        m_highAccuracy = enabled;
        if (m_updating)
            updateSource(m_view);
    }

    std::optional<GeolocationPositionData> lastPosition() final { return lastKnownPosition(); }

    void requestPermission(Geolocation& geolocation) final
    {
        RefPtr document = geolocation.document();
        if (!document) {
            geolocation.setIsAllowed(false, { });
            return;
        }
        m_asking.append(&geolocation);
        ask(m_view, LW_PERMISSION_GEOLOCATION, document->securityOrigin().toString(), [client = Ref { *this }, geolocation = Ref { geolocation }](bool allowed) {
            // Not if the page took the request back meanwhile (cancelPermissionRequest), or went away.
            if (client->m_asking.removeFirst(geolocation.ptr()))
                geolocation->setIsAllowed(allowed, { });
        });
    }

    void cancelPermissionRequest(Geolocation& geolocation) final { m_asking.removeFirst(&geolocation); }

    // The source follows what the watching pages need; the app hears of it only when that changes.
    static void updateSource(lw_view* view)
    {
        auto& source = positionSource();
        int wanted = 0;
        for (auto* client : geolocationClients()) {
            if (client->m_updating)
                wanted = std::max(wanted, client->m_highAccuracy ? 2 : 1);
        }
        if (wanted == source.accuracy || !view)
            return;
        if (!source.accuracy)
            source.hasPosition = false;
        source.accuracy = wanted;
        if (!source.repeat)
            source.repeat = makeUnique<RunLoop::Timer>(RunLoop::mainSingleton(), "PositionRepeat"_s, [] { repeatPosition(); });
        if (wanted && !source.repeat->isActive())
            source.repeat->startRepeating(5_s);
        else if (!wanted)
            source.repeat->stop();
        log("posizione: sorgente %d", wanted);
        postEvent(*view, LW_EVENT_GEOLOCATION, wanted);
    }

    lw_view* m_view;
    Page* m_page;
    Vector<Geolocation*> m_asking;
    bool m_updating { false };
    bool m_highAccuracy { false };
};

Vector<Ref<GeolocationClientWP81>> updatingGeolocationClients()
{
    Vector<Ref<GeolocationClientWP81>> clients;
    for (auto* client : geolocationClients()) {
        if (client->isUpdating() && client->page())
            clients.append(*client);
    }
    return clients;
}

void deliverPosition(const GeolocationPositionData& position)
{
    auto& source = positionSource();
    source.hasPosition = true;
    source.lastDelivery = MonotonicTime::now();
    lastKnownPosition() = position;
    for (auto& client : updatingGeolocationClients()) {
        if (auto* controller = client->page() ? GeolocationController::from(client->page()) : nullptr)
            controller->positionChanged(position);
    }
}

// ---- notifications ----

class NotificationClientWP81;

Vector<NotificationClientWP81*>& notificationClients()
{
    static NeverDestroyed<Vector<NotificationClientWP81*>> clients;
    return clients;
}

// The notifications the app may still report a tap on (the last 32).
struct ShownNotification {
    int id { 0 };
    lw_view* view { nullptr };
    NotificationData data;
};

Vector<ShownNotification>& shownNotifications()
{
    static NeverDestroyed<Vector<ShownNotification>> shown;
    return shown;
}

// The page's Notification object hears of it in a task of its own, as when the answer comes from another process.
void dispatchLater(const NotificationData& data, void (Notification::*event)())
{
    if (!data.contextIdentifier)
        return;
    postToWebThread([data = data.isolatedCopy(), event] {
        Notification::ensureOnNotificationThread(data, [event](Notification* notification) {
            if (notification)
                (notification->*event)();
        });
    });
}

class NotificationClientWP81 final : public NotificationClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NotificationClientWP81);
public:
    explicit NotificationClientWP81(lw_view& view)
        : m_view(&view)
    {
        notificationClients().append(this);
    }
    ~NotificationClientWP81() { notificationClients().removeFirst(this); }

    lw_view* view() const { return m_view; }
    void viewWillClose() { m_view = nullptr; }

private:
    bool show(ScriptExecutionContext& context, NotificationData&& data, RefPtr<NotificationResources>&&, CompletionHandler<void()>&& done) final
    {
        if (!m_view || checkPermission(&context) != Permission::Granted) {
            done();
            return false;
        }
        static int lastID;
        int id = ++lastID;
        auto& shown = shownNotifications();
        if (shown.size() >= 32)
            shown.removeAt(0);
        shown.append(ShownNotification { id, m_view, data });
        log("notifica %d: %s", id, data.originString.utf8().data());
        postEvent(*m_view, LW_EVENT_NOTIFICATION, 1, data.title, data.body, id);
        dispatchLater(data, &Notification::dispatchShowEvent);
        done();
        return true;
    }

    void cancel(NotificationData&& data) final
    {
        auto& shown = shownNotifications();
        auto index = shown.findIf([&](auto& entry) { return entry.data.notificationID == data.notificationID; });
        if (index == notFound)
            return;
        auto entry = shown[index];
        shown.removeAt(index);
        if (entry.view)
            postEvent(*entry.view, LW_EVENT_NOTIFICATION, 0, { }, { }, entry.id);
        dispatchLater(data, &Notification::dispatchCloseEvent);
    }

    // The notification stays on the phone; a tap on it has no object to tell any more.
    void notificationObjectDestroyed(NotificationData&& data) final
    {
        shownNotifications().removeAllMatching([&](auto& entry) { return entry.data.notificationID == data.notificationID; });
    }

    void notificationControllerDestroyed() final { delete this; }

    void requestPermission(ScriptExecutionContext& context, PermissionHandler&& handler) final
    {
        RefPtr origin = context.securityOrigin();
        if (!origin) {
            handler(Permission::Denied);
            return;
        }
        ask(m_view, LW_PERMISSION_NOTIFICATIONS, origin->toString(), [handler = WTF::move(handler)](bool allowed) mutable {
            handler(allowed ? Permission::Granted : Permission::Denied);
        });
    }

    Permission checkPermission(ScriptExecutionContext* context) final
    {
        RefPtr origin = context ? context->securityOrigin() : nullptr;
        if (!origin)
            return Permission::Denied;
        auto known = decided(m_view, LW_PERMISSION_NOTIFICATIONS, origin->toString());
        if (!known)
            return Permission::Default;
        return *known ? Permission::Granted : Permission::Denied;
    }

    lw_view* m_view;
};

} // namespace

void provideDeviceFeatures(lw_view& view, Page& page)
{
    provideGeolocationTo(&page, GeolocationClientWP81::create(view, page));
    // Its NotificationController deletes it (notificationControllerDestroyed).
    provideNotification(&page, new NotificationClientWP81(view));
}

void deviceFeaturesViewWillClose(lw_view& view)
{
    Vector<int> ids;
    for (auto& entry : questions()) {
        if (entry.value.view == &view)
            ids.append(entry.key);
    }
    for (int id : ids)
        answer(id, std::nullopt);
    for (auto& client : copyToVector(geolocationClients())) {
        if (client->view() == &view)
            client->viewWillClose();
    }
    for (auto* client : notificationClients()) {
        if (client->view() == &view)
            client->viewWillClose();
    }
    shownNotifications().removeAllMatching([&](auto& entry) { return entry.view == &view; });
}

void share(lw_view& view, const ShareDataWithParsedURL& data, CompletionHandler<void(bool)>&& done)
{
    if (!isViewActive(view)) {
        done(false);
        return;
    }
    // The words (title, then text) and the address apart: the app shares a link as a link.
    auto& title = data.shareData.title;
    auto& text = data.shareData.text;
    auto words = title.isEmpty() || text.isEmpty() ? makeString(title, text) : makeString(title, '\n', text);
    postEvent(view, LW_EVENT_SHARE, 0, words, data.url ? data.url->string() : String());
    done(true);
}

} // namespace Lumia

void lw_permission_reply(lw_view*, int id, int allowed)
{
    Lumia::postToWebThread([id, allowed] {
        if (Lumia::questions().contains(id))
            Lumia::answer(id, allowed < 0 ? std::nullopt : std::optional<bool> { allowed != 0 });
    }, true);
}

static std::optional<double> optionalNumber(double value)
{
    if (std::isnan(value))
        return std::nullopt;
    return value;
}

void lw_geolocation_update(double latitude, double longitude, double accuracy, double altitude, double altitudeAccuracy, double heading, double speed)
{
    Lumia::postToWebThread([=] {
        WebCore::GeolocationPositionData position(WallTime::now().secondsSinceEpoch().seconds(), latitude, longitude, accuracy,
            optionalNumber(altitude), optionalNumber(altitudeAccuracy), optionalNumber(heading), optionalNumber(speed), std::nullopt);
        Lumia::deliverPosition(position);
    });
}

void lw_geolocation_error(int code)
{
    Lumia::postToWebThread([code] {
        Lumia::log("posizione: errore %d", code);
        for (auto& client : Lumia::updatingGeolocationClients()) {
            auto* controller = client->page() ? WebCore::GeolocationController::from(client->page()) : nullptr;
            if (!controller)
                continue;
            Ref error = code == 1
                ? WebCore::GeolocationError::create(WebCore::GeolocationError::PermissionDenied, "Location is turned off on the phone"_s)
                : WebCore::GeolocationError::create(WebCore::GeolocationError::PositionUnavailable, "The phone could not find its position"_s);
            controller->errorOccurred(error);
        }
    });
}

void lw_notification_clicked(lw_view*, int id)
{
    Lumia::postToWebThread([id] {
        auto& shown = Lumia::shownNotifications();
        auto index = shown.findIf([&](auto& entry) { return entry.id == id; });
        if (index == notFound)
            return;
        auto data = shown[index].data;
        shown.removeAt(index);
        Lumia::dispatchLater(data, &WebCore::Notification::dispatchClickEvent);
    }, true);
}
