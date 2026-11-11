#pragma once

#include "MainPage.g.h"
#include "ViewModels.h"

#include <collection.h>
#include <vector>

namespace Telegram
{
    class MainListener;

    public ref class MainPage sealed
    {
    public:
        MainPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    internal:
        void Chat_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        Platform::Collections::Vector<ChatVM^>^ chats_;
        MainListener* listener_;
        bool active_;

        void RefreshView();
        void UpdateConnectionStatus();
        void ChatList_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void Refresh_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void NewChat_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Settings_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    };
}
