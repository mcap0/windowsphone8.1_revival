#include "pch.h"
#include "MainPage.xaml.h"
#include "..\Core\SearchHelpers.h"

using namespace Cerca;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::System;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::Web::Http;
using namespace concurrency;

namespace
{
    std::string ToUtf8(String^ value)
    {
        if (value == nullptr || value->IsEmpty())
            return std::string();

        std::wstring wide(value->Data());
        int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1)
            return std::string();

        std::vector<char> bytes(required);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &bytes[0], required, nullptr, nullptr);
        return std::string(bytes.begin(), bytes.end() - 1);
    }

    String^ FromUtf8(const std::string& value)
    {
        if (value.empty())
            return ref new String(L"");

        int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (required <= 0)
            return ref new String(L"");

        std::wstring wide(required, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &wide[0], required);
        return ref new String(wide.c_str(), static_cast<unsigned int>(wide.size()));
    }

    String^ ToPlatformString(const std::string& value)
    {
        return FromUtf8(value);
    }

    String^ UriString(const std::string& value)
    {
        return FromUtf8(value);
    }
}

MainPage::MainPage()
    : _requestGeneration(0)
{
    InitializeComponent();
    _suggestions = ref new Platform::Collections::Vector<String^>();
    SuggestionsList->ItemsSource = _suggestions;

    _httpClient = ref new HttpClient();
    _debounceTimer = ref new DispatcherTimer();
    TimeSpan interval;
    interval.Duration = 2500000;
    _debounceTimer->Interval = interval;
    _debounceTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::OnDebounceTick);
}

void MainPage::Page_Loaded(Object^, RoutedEventArgs^)
{
    SearchBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::SearchBox_TextChanged(Object^, TextChangedEventArgs^)
{
    ++_requestGeneration;
    _debounceTimer->Stop();
    _suggestions->Clear();

    if (SearchBox->Text == nullptr || SearchBox->Text->IsEmpty())
        return;

    _debounceTimer->Start();
}

void MainPage::SearchBox_KeyDown(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key == VirtualKey::Enter)
    {
        e->Handled = true;
        _debounceTimer->Stop();
        OpenSearch(SearchBox->Text);
    }
}

void MainPage::Suggestions_ItemClick(Object^, ItemClickEventArgs^ e)
{
    auto suggestion = dynamic_cast<String^>(e->ClickedItem);
    if (suggestion != nullptr)
    {
        _debounceTimer->Stop();
        OpenSearch(suggestion);
    }
}

void MainPage::OnDebounceTick(Object^, Object^)
{
    _debounceTimer->Stop();
    RequestSuggestions();
}

void MainPage::RequestSuggestions()
{
    String^ text = SearchBox->Text;
    if (text == nullptr || text->IsEmpty())
        return;

    const unsigned long long generation = _requestGeneration;
    const std::string query = ToUtf8(text);
    if (query.empty())
        return;

    const std::string requestUrl =
        "http://api.bing.com/qsonhs.aspx?q=" + CercaCore::UrlEncode(query) +
        "&form=MO0014&mkt=it-IT";

    try
    {
        auto uri = ref new Uri(UriString(requestUrl));
        auto operation = _httpClient->GetStringAsync(uri);

        create_task(operation).then(
            [this, generation](task<String^> result)
            {
                try
                {
                    String^ json = result.get();
                    if (generation != _requestGeneration || json == nullptr)
                        return;

                    std::wstring body(json->Data());
                    std::string utf8;
                    if (!body.empty())
                    {
                        int required = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, nullptr, 0, nullptr, nullptr);
                        if (required > 1)
                        {
                            std::vector<char> bytes(required);
                            WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, &bytes[0], required, nullptr, nullptr);
                            utf8.assign(bytes.begin(), bytes.end() - 1);
                        }
                    }

                    const std::vector<std::string> parsed = CercaCore::ParseSuggestions(utf8);
                    _suggestions->Clear();
                    for (size_t i = 0; i < parsed.size() && i < 8; ++i)
                        _suggestions->Append(ToPlatformString(parsed[i]));
                }
                catch (Exception^)
                {
                    // La connessione può non essere disponibile: la casella resta utilizzabile.
                }
            },
            task_continuation_context::use_current());
    }
    catch (Exception^)
    {
        // Uri o richiesta non valida; non interrompere l'interazione.
    }
}

void MainPage::OpenSearch(String^ text)
{
    if (text == nullptr || text->IsEmpty())
        return;

    const std::string query = ToUtf8(text);
    if (query.empty())
        return;

    if (CercaCore::LooksLikeUrl(query))
    {
        LaunchMetroSurf(query);
        return;
    }

    const std::string target =
        "https://www.google.com/search?q=" + CercaCore::UrlEncode(query);
    LaunchMetroSurf(target);
}

void MainPage::LaunchMetroSurf(const std::string& target)
{
    const std::string uriText = "metrosurf://" + target;

    try
    {
        auto uri = ref new Uri(UriString(uriText));
        Windows::System::Launcher::LaunchUriAsync(uri);
    }
    catch (Exception^)
    {
        // Lascia l'app aperta se il testo non forma un Uri valido.
    }
}
