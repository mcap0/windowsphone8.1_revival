#pragma once
#include "MainPage.g.h"

namespace Avvisi
{
    public ref class MainPage sealed
    {
    public:
        MainPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void OnStart(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnStop(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Refresh();
        void RegisterWatch();
        void Send(Platform::String^ command);

        Windows::UI::Xaml::DispatcherTimer^ timer_;
    };
}
