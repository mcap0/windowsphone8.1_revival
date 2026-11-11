#include "Net.h"
#include "Timeout.h"
#include "WinRtText.h"

#include <collection.h>
#include <ppltasks.h>
#include <map>
#include <string>
#include <windows.foundation.collections.h>
#include <windows.web.http.h>
#include <windows.web.http.headers.h>

using namespace Windows::Foundation;
using namespace Windows::Web::Http;

namespace PerleCast {

namespace {

HttpResult ReadResponse(HttpResponseMessage^ response)
{
    HttpResult result;
    result.code = static_cast<int>(response->StatusCode);
    if (response->Content != nullptr)
    {
        Platform::String^ text =
            WaitAtMost(response->Content->ReadAsStringAsync(), 10);
        result.body = ToUtf8(text);
    }

    for each (Windows::Foundation::Collections::IKeyValuePair<
        Platform::String^, Platform::String^>^ header in response->Headers)
    {
        result.headers[ToUtf8(header->Key)] = ToUtf8(header->Value);
    }

    return result;
}

HttpClient^ NewClient()
{
    HttpClient^ client = ref new HttpClient();
    client->DefaultRequestHeaders->Append(
        ref new Platform::String(L"User-Agent"),
        ref new Platform::String(
            L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    return client;
}

} // namespace

HttpResult Net::Post(const std::string& url,
                     const std::map<std::string, std::string>& headers,
                     const std::string& body)
{
    HttpClient^ client = NewClient();
    HttpRequestMessage^ request = ref new HttpRequestMessage(
        HttpMethod::Post, ref new Uri(FromUtf8(url)));

    for (std::map<std::string, std::string>::const_iterator it = headers.begin();
         it != headers.end(); ++it)
    {
        try
        {
            request->Headers->Append(FromUtf8(it->first), FromUtf8(it->second));
        }
        catch (Platform::Exception^)
        {
            // Some SOAP headers are not registered as known HTTP headers on WP8.1.
        }
    }

    request->Content = ref new HttpStringContent(
        FromUtf8(body), Windows::Storage::Streams::UnicodeEncoding::Utf8,
        ref new Platform::String(L"text/xml; charset=\"utf-8\""));

    HttpResponseMessage^ response =
        WaitAtMost(client->SendRequestAsync(request), 10);
    return ReadResponse(response);
}

HttpResult Net::Get(const std::string& url)
{
    HttpClient^ client = NewClient();
    HttpResponseMessage^ response =
        WaitAtMost(client->GetAsync(ref new Uri(FromUtf8(url))), 10);
    return ReadResponse(response);
}

} // namespace PerleCast
