#pragma once

#include "MainPage.g.h"
#include <map>
#include <string>

namespace Risultati
{
    public ref class MainPage sealed
    {
    public:
        MainPage();
        void Start(Platform::String^ args);

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void Search(const std::wstring& query);
        void SearchWeb(const std::wstring& query);
        void SearchBox_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void NavigateGoogle(Windows::Foundation::Uri^ uri);
        void Web_NavigationStarting(Windows::UI::Xaml::Controls::WebView^ sender, Windows::UI::Xaml::Controls::WebViewNavigationStartingEventArgs^ e);
        void Web_NavigationCompleted(Windows::UI::Xaml::Controls::WebView^ sender, Windows::UI::Xaml::Controls::WebViewNavigationCompletedEventArgs^ e);
        void OnPoll(Platform::Object^ sender, Platform::Object^ e);
        void Extract();
        void Render(Windows::Data::Json::JsonObject^ data);
        void AddOverview(Windows::Data::Json::JsonArray^ blocks);
        void AddResult(Platform::String^ title, Platform::String^ url, Platform::String^ cite, Platform::String^ snippet);
        void Open(Platform::String^ url);
        static bool IsWrapper(Platform::String^ url);
        concurrency::task<Platform::String^> Unwrap(Platform::String^ url);
        void ShowLink(Windows::UI::Xaml::FrameworkElement^ target, Platform::String^ url);
        void Finish(Platform::String^ message);
        void Log(const std::wstring& text);
        void DumpPage();
        void SendShot();
        void InjectPolyfill();
        void SetStatus(const std::wstring& lead, const std::wstring& query, const std::wstring& tail);
        void ShowProgress(bool on);
        void ShowSearchBarAtBottom(bool bottom);
        void Mic_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        // sections as in the old Bing app: web / immagini / locale
        void Sections_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void LoadImages();
        void ShowImage(Platform::String^ full, Platform::String^ page, Platform::String^ title);
        void CloseImage();
        void SearchLocal();
        void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);

        Windows::UI::Xaml::DispatcherTimer^ _poll;
        std::wstring _query;
        std::wstring _lastLog;
        Platform::String^ _ownNavigation;
        std::wstring _shown;     // signature of what is on screen, to redraw only on changes
        bool _noResults;         // page loaded, AI found but no result links
        bool _aiExpanded;        // "mostra altro" tapped: keep the whole overview on redraws
        unsigned _ticks;
        unsigned _stable;
        unsigned _generation;
        bool _busy;
        std::map<std::wstring, std::wstring> _real;  // Google /url wrapper -> real address
        bool _localStarted;                          // "locale" searched for this query
        bool _imagesStarted;                         // "immagini" requested for this query
        void StartImages();
        void AddContacts(unsigned generation);
        Windows::UI::Xaml::Controls::Grid^ _viewer;  // full-size image over everything
    };
}
