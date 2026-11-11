#pragma once

#include "SettingsPage.g.h"

namespace Telegram
{
    public ref class SettingsPage sealed
    {
    public:
        SettingsPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void SendLog_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Logout_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    };
}
