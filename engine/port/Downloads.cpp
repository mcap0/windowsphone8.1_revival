#include "config.h"
#include "Downloads.h"

#include "Engine.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/AuthenticationClient.h>
#include <WebCore/DataURLDecoder.h>
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceHandle.h>
#include <WebCore/ResourceHandleClient.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefCounted.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace Lumia {
using namespace WebCore;

namespace {

enum class Result { Done, Failed, Cancelled };

class Download;

HashMap<int, Ref<Download>>& downloads()
{
    static NeverDestroyed<HashMap<int, Ref<Download>>> map;
    return map;
}

// "%20" and the like in the last part of an address, as UTF-8.
String decodePercentEscapes(StringView text)
{
    Vector<uint8_t> bytes;
    auto hex = [](char16_t c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (unsigned i = 0; i < text.length(); ++i) {
        char16_t c = text[i];
        if (c == '%' && i + 2 < text.length() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            bytes.append(static_cast<uint8_t>(hex(text[i + 1]) * 16 + hex(text[i + 2])));
            i += 2;
        } else if (c < 0x80)
            bytes.append(static_cast<uint8_t>(c));
        else
            return text.toString();
    }
    String decoded = String::fromUTF8(bytes.span());
    return decoded.isNull() ? text.toString() : decoded;
}

// What the file is called on disk: the name it was given, without characters Windows refuses, and a number added while
// the name is taken.
String uniquePath(const String& directory, const String& suggested)
{
    StringBuilder clean;
    for (auto character : StringView(suggested).codeUnits()) {
        if (character < 32 || character == '\\' || character == '/' || character == ':' || character == '*' || character == '?'
            || character == '"' || character == '<' || character == '>' || character == '|')
            clean.append('_');
        else
            clean.append(character);
    }
    String name = clean.toString().trim(isASCIIWhitespace);
    if (name.isEmpty() || name == "."_s || name == ".."_s)
        name = "download"_s;
    if (name.length() > 120)
        name = name.right(120);
    size_t dot = name.reverseFind('.');
    String stem = dot == notFound || !dot ? name : name.left(dot);
    String extension = dot == notFound || !dot ? String() : name.substring(dot);
    String path = FileSystem::pathByAppendingComponent(directory, name);
    for (unsigned number = 1; FileSystem::fileExists(path) && number < 1000; ++number)
        path = FileSystem::pathByAppendingComponent(directory, makeString(stem, " ("_s, number, ')', extension));
    return path;
}

class Download final : public ResourceHandleClient, public RefCounted<Download> {
public:
    static Ref<Download> create(lw_view& view, const URL& url, const String& suggestedName)
    {
        static int lastID;
        return adoptRef(*new Download(view, ++lastID, url, suggestedName));
    }

    int id() const { return m_id; }

    // The page's own load: the response is known, and the body follows through the handle the loader lets go of.
    void adopt(ResourceHandle& handle, const ResourceResponse& response)
    {
        Ref protectedThis { *this };
        if (!begin(response)) {
            handle.cancel();
            return;
        }
        m_handle = &handle;
        handle.setClient(this);
    }

    // A load of its own (http, https, blob), with the cookies of the context's session.
    void load(NetworkingContext* context, const ResourceRequest& request)
    {
        Ref protectedThis { *this };
        m_handle = ResourceHandle::create(context, request, this, false, false, ContentEncodingSniffingPolicy::Default, nullptr, false);
        if (!m_handle)
            finish(Result::Failed, "la richiesta non parte"_s);
    }

    // data: carries its bytes in the address.
    void saveData(const URL& url)
    {
        Ref protectedThis { *this };
        auto result = DataURLDecoder::decode(url);
        if (!result) {
            finish(Result::Failed, "dati non validi"_s);
            return;
        }
        ResourceResponse response(URL { url }, String { result->mimeType }, static_cast<long long>(result->data.size()), String { result->charset });
        if (!begin(response))
            return;
        write(result->data.span());
        finish(Result::Done, { });
    }

    void fail(const String& error) { finish(Result::Failed, error); }
    void cancel() { finish(Result::Cancelled, { }); }

private:
    Download(lw_view& view, int id, const URL& url, const String& suggestedName)
        : m_view(&view)
        , m_id(id)
        , m_url(url)
        , m_suggestedName(suggestedName)
    {
    }

    // ResourceHandleClient: only loads of our own get the redirects and the response; an adopted load is past both.
    void willSendRequestAsync(ResourceHandle*, ResourceRequest&& request, ResourceResponse&&, CompletionHandler<void(ResourceRequest&&)>&& completionHandler) final
    {
        m_url = request.url();
        completionHandler(WTF::move(request));
    }

    void didReceiveResponseAsync(ResourceHandle*, ResourceResponse&& response, CompletionHandler<void()>&& completionHandler) final
    {
        Ref protectedThis { *this };
        begin(response);
        completionHandler();
    }

    void didReceiveBuffer(ResourceHandle*, const FragmentedSharedBuffer& buffer, int) final
    {
        Ref protectedThis { *this };
        bool written = true;
        buffer.forEachSegment([&](std::span<const uint8_t> segment) {
            if (written)
                written = write(segment);
        });
        if (!written) {
            finish(Result::Failed, "memoria del telefono piena o file non scrivibile"_s);
            return;
        }
        // At most two progress events a second.
        auto now = MonotonicTime::now();
        if (now - m_lastProgress >= 500_ms) {
            m_lastProgress = now;
            post(LW_EVENT_DOWNLOAD_PROGRESS, static_cast<int64_t>(m_received), { }, { });
        }
    }

    void didFinishLoading(ResourceHandle*, const NetworkLoadMetrics&) final
    {
        Ref protectedThis { *this };
        finish(Result::Done, { });
    }

    void didFail(ResourceHandle*, const ResourceError& error) final
    {
        Ref protectedThis { *this };
        finish(Result::Failed, error.localizedDescription().isEmpty() ? "errore di rete"_s : error.localizedDescription());
    }

    void wasBlocked(ResourceHandle*) final { fail("bloccato"_s); }
    void cannotShowURL(ResourceHandle*) final { fail("indirizzo non valido"_s); }
    bool shouldUseCredentialStorage(ResourceHandle*) final { return true; }
    // The password the page's site already took comes from the session (ResourceHandleCurl.cpp); a download does not
    // ask for another: the 401 is its answer, and it fails as any HTTP error.
    void didReceiveAuthenticationChallenge(ResourceHandle*, const AuthenticationChallenge& challenge) final
    {
        if (RefPtr client = challenge.authenticationClient())
            client->receivedRequestToContinueWithoutCredential(challenge);
    }
#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
    void canAuthenticateAgainstProtectionSpaceAsync(ResourceHandle*, const ProtectionSpace&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
#endif

    // The response is in: the file gets its name and is created, and the app hears of the download.
    bool begin(const ResourceResponse& response)
    {
        if (m_finished)
            return false;
        if (response.isInHTTPFamily() && response.httpStatusCode() >= 400) {
            finish(Result::Failed, makeString("il sito ha risposto "_s, response.httpStatusCode()));
            return false;
        }
        String directory;
        if (auto* downloadDirectory = config().download_directory)
            directory = String(downloadDirectory);
        else if (auto* dataDirectory = config().data_directory)
            directory = FileSystem::pathByAppendingComponent(String(dataDirectory), "Downloads"_s);
        if (directory.isEmpty()) {
            finish(Result::Failed, "nessuna cartella per i download"_s);
            return false;
        }
        if (!response.url().isEmpty())
            m_url = response.url();
        FileSystem::makeAllDirectories(directory);
        m_path = uniquePath(directory, fileName(response));
        m_file = FileSystem::openFile(m_path, FileSystem::FileOpenMode::Truncate);
        if (!m_file) {
            finish(Result::Failed, "impossibile creare il file"_s);
            return false;
        }
        if (response.expectedContentLength() > 0)
            m_expected = response.expectedContentLength();
        log("download %d: %s -> %s (%lld byte attesi)", m_id, m_url.string().utf8().data(), m_path.utf8().data(), m_expected);
        m_announced = true;
        post(LW_EVENT_DOWNLOAD, m_expected, m_url.string(), m_path);
        return true;
    }

    // The download attribute, else the server's name (Content-Disposition), else the last part of the address; a name
    // without an extension gets its type's, so the phone knows which app opens it.
    String fileName(const ResourceResponse& response) const
    {
        String name = m_suggestedName;
        if (name.isEmpty())
            name = response.suggestedFilename();
        if (name.isEmpty() && !m_url.protocolIsData() && !m_url.protocolIsBlob())
            name = decodePercentEscapes(m_url.lastPathComponent());
        if (name.isEmpty())
            name = "download"_s;
        if (name.find('.') == notFound) {
            String extension = MIMETypeRegistry::preferredExtensionForMIMEType(response.mimeType());
            if (!extension.isEmpty())
                name = makeString(name, '.', extension);
        }
        return name;
    }

    bool write(std::span<const uint8_t> bytes)
    {
        if (!m_file)
            return false;
        if (m_file.write(bytes) != bytes.size())
            return false;
        m_received += bytes.size();
        return true;
    }

    // Downloads outlive their tab: once the view is gone the events go out with no view.
    void post(lw_event_type type, int64_t number, const String& text, const String& text2)
    {
        auto& config = Lumia::config();
        if (!config.callback)
            return;
        auto characters = text.charactersWithNullTermination();
        auto characters2 = text2.charactersWithNullTermination();
        lw_event event { };
        event.type = type;
        event.id = m_id;
        event.number = number;
        event.text = characters ? reinterpret_cast<const wchar_t*>(characters->span().data()) : L"";
        event.text2 = characters2 ? reinterpret_cast<const wchar_t*>(characters2->span().data()) : L"";
        config.callback(config.context, liveWebPage(m_view) ? m_view : nullptr, &event);
    }

    // A file that did not arrive whole is removed. Once.
    void finish(Result result, const String& error)
    {
        if (std::exchange(m_finished, true))
            return;
        Ref protectedThis { *this };
        if (RefPtr handle = std::exchange(m_handle, nullptr)) {
            handle->clearClient();
            if (result != Result::Done)
                handle->cancel();
        }
        m_file = { };
        if (result != Result::Done && !m_path.isEmpty())
            FileSystem::deleteFile(m_path);
        int code = result == Result::Done ? 0 : result == Result::Failed ? 1 : 2;
        log("download %d: %s, %llu byte%s%s", m_id, code == 0 ? "completato" : code == 1 ? "non riuscito" : "annullato",
            m_received, error.isEmpty() ? "" : ", ", error.utf8().data());
        // One that failed before its file existed is still announced, so the app can say what happened.
        if (!m_announced)
            post(LW_EVENT_DOWNLOAD, -1, m_url.string(), { });
        post(LW_EVENT_DOWNLOAD_FINISHED, code, m_path, error);
        downloads().remove(m_id);
    }

    lw_view* m_view;
    int m_id;
    URL m_url;
    String m_suggestedName;
    String m_path;
    FileSystem::FileHandle m_file;
    RefPtr<ResourceHandle> m_handle;
    long long m_expected { -1 };
    unsigned long long m_received { 0 };
    MonotonicTime m_lastProgress;
    bool m_announced { false };
    bool m_finished { false };
};

} // namespace

void convertToDownload(lw_view& view, ResourceHandle& handle, const ResourceRequest& request, const ResourceResponse& response)
{
    Ref download = Download::create(view, response.url().isEmpty() ? request.url() : response.url(), { });
    downloads().add(download->id(), download.copyRef());
    download->adopt(handle, response);
}

void startDownload(lw_view& view, NetworkingContext* context, const ResourceRequest& originalRequest, const String& suggestedName)
{
    ResourceRequest request = originalRequest;
    URL url = request.url();
    Ref download = Download::create(view, url, suggestedName.isEmpty() ? String() : ResourceResponse::sanitizeSuggestedFilename(suggestedName));
    downloads().add(download->id(), download.copyRef());
    if (url.protocolIsData()) {
        download->saveData(url);
        return;
    }
    if (!url.protocolIsInHTTPFamily() && !url.protocolIsBlob()) {
        download->fail(makeString("indirizzo "_s, url.protocol(), ": non scaricabile"_s));
        return;
    }
    if (url.protocolIsInHTTPFamily() && request.httpUserAgent().isEmpty())
        request.setHTTPUserAgent(userAgent(view, url));
    download->load(context, request);
}

void cancelDownload(int id)
{
    if (RefPtr download = downloads().get(id))
        download->cancel();
}

} // namespace Lumia
