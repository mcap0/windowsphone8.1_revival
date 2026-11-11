#pragma once

#include "NewChatPage.g.h"
#include "client.h"

namespace Telegram
{
    [Windows::UI::Xaml::Data::Bindable]
    public ref class PersonEntry sealed
    {
    public:
        property Platform::String^ Name { Platform::String^ get() { return name_; } }
        property Platform::String^ Detail { Platform::String^ get() { return detail_; } }
        property Platform::String^ Key { Platform::String^ get() { return key_; } }

    internal:
        PersonEntry(Platform::String^ key, Platform::String^ name, Platform::String^ detail) : key_(key), name_(name), detail_(detail) {}

    private:
        Platform::String^ key_;
        Platform::String^ name_;
        Platform::String^ detail_;
    };

    // Address book filtered while typing; after a pause (or Enter) also asks Telegram for public users and
    // channels matching the text. Tapping a person opens ChatPage on that key.
    // Navigated with "inoltra": lists recent chats first and forwards TgService::pendingForward to the one tapped.
    public ref class NewChatPage sealed
    {
    public:
        NewChatPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    internal:
        void Query_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void Query_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void People_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);

    private:
        void Show();
        void SearchNow();

        std::vector<tg::ContactItem> contacts_;
        std::vector<tg::ContactItem> found_;
        std::string foundFor_;
        bool active_;
        bool forward_;
        Windows::UI::Xaml::DispatcherTimer^ timer_;
    };
}
