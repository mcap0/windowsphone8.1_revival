#pragma once

#include "MainPage.g.h"
#include <string>

namespace Cerca
{
    public ref class MainPage sealed
    {
    public:
        MainPage();

    private:
        void Page_Loaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void SearchBox_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void SearchBox_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void Suggestions_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnDebounceTick(Platform::Object^ sender, Platform::Object^ e);

        void RequestSuggestions();
        void OpenSearch(Platform::String^ text);
        void LaunchMetroSurf(const std::string& target);

        Windows::UI::Xaml::DispatcherTimer^ _debounceTimer;
        Windows::Web::Http::HttpClient^ _httpClient;
        Windows::Foundation::Collections::IObservableVector<Platform::String^>^ _suggestions;
        unsigned long long _requestGeneration;
    };
}
