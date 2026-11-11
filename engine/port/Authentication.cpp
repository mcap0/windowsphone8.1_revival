#include "config.h"
#include "Authentication.h"

#include "Engine.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/AuthenticationClient.h>
#include <WebCore/Credential.h>
#include <WebCore/ProtectionSpace.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

namespace Lumia {
using namespace WebCore;

namespace {

// One question on the app's screen, and the loads that wait for its answer: a page whose style sheets and images sit
// behind the same password asks once, not once per file.
struct Question {
    lw_view* view { nullptr };
    String place;
    Vector<AuthenticationChallenge> challenges;
};

HashMap<int, Question>& questions()
{
    static NeverDestroyed<HashMap<int, Question>> map;
    return map;
}

void continueWithoutPassword(const AuthenticationChallenge& challenge)
{
    if (RefPtr client = challenge.authenticationClient())
        client->receivedRequestToContinueWithoutCredential(challenge);
}

void answer(int id, std::optional<Credential>&& credential)
{
    auto question = questions().take(id);
    log("password %d: %s, %u caricamenti", id, credential ? "data" : "annullata", static_cast<unsigned>(question.challenges.size()));
    for (auto& challenge : question.challenges) {
        RefPtr client = challenge.authenticationClient();
        if (!client)
            continue;
        if (credential)
            client->receivedCredential(challenge, *credential);
        else
            client->receivedRequestToContinueWithoutCredential(challenge);
    }
}

String fromWide(const wchar_t* text)
{
    return String(std::span<const char16_t> { reinterpret_cast<const char16_t*>(text), wcslen(text) }).isolatedCopy();
}

} // namespace

void askForPassword(lw_view& view, const AuthenticationChallenge& challenge)
{
    auto& space = challenge.protectionSpace();
    // A tab nobody sees cannot ask: the page gets the server's 401, and asks again when it is reloaded in view.
    if (!isViewActive(view)) {
        log("password: scheda nascosta, %s senza password", space.host().utf8().data());
        continueWithoutPassword(challenge);
        return;
    }
    auto url = challenge.failureResponse().url();
    // What the user is told the password is for: the server as in the address bar. The realm is the server's own
    // text and goes apart (text2), so it cannot pass for the address.
    auto place = makeString(url.protocol(), "://"_s, url.hostAndPort());
    auto key = makeString(place, '\n', space.realm());
    // A refused password asks again by itself (previousFailureCount), not through a question that is still open.
    if (!challenge.previousFailureCount()) {
        for (auto& question : questions().values()) {
            if (question.place == key) {
                question.challenges.append(challenge);
                return;
            }
        }
    }
    static int lastID;
    int id = ++lastID;
    questions().set(id, Question { &view, key, { challenge } });
    log("password %d: %s chiede (schema %d, rifiuti %u)", id, place.utf8().data(), static_cast<int>(space.authenticationScheme()), challenge.previousFailureCount());
    postEvent(view, LW_EVENT_PASSWORD, challenge.previousFailureCount(), place, space.realm(), id);
}

void cancelPasswordQuestions(lw_view& view)
{
    Vector<int> ids;
    for (auto& entry : questions()) {
        if (entry.value.view == &view)
            ids.append(entry.key);
    }
    for (int id : ids)
        answer(id, std::nullopt);
}

} // namespace Lumia

void lw_password_reply(lw_view*, int id, const wchar_t* user, const wchar_t* password)
{
    std::optional<WebCore::Credential> credential;
    if (user) {
        // Kept until the app ends (the session's CredentialStorage): the rest of the site loads without asking.
        credential = WebCore::Credential(Lumia::fromWide(user), Lumia::fromWide(password ? password : L""), WebCore::CredentialPersistence::ForSession);
    }
    Lumia::postToWebThread([id, credential = WTF::move(credential)]() mutable {
        if (Lumia::questions().contains(id))
            Lumia::answer(id, WTF::move(credential));
    }, true);
}
