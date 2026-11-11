#pragma once

#include "LoginPage.g.h"

namespace Telegram
{
    public ref class LoginPage sealed
    {
    public:
        LoginPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        bool busy_;
        void Continue_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ShowAuthState();
        void ShowError(const std::string& error);
        void CompleteLogin();
    };
}
