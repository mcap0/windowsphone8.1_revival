#pragma once

#include "TopicsPage.g.h"
#include "client.h"

#include <collection.h>

namespace Telegram
{
    [Windows::UI::Xaml::Data::Bindable]
    public ref class TopicEntry sealed
    {
    public:
        property Platform::String^ Title { Platform::String^ get() { return title_; } }
        property Platform::String^ Preview { Platform::String^ get() { return preview_; } }
        property Platform::String^ Unread { Platform::String^ get() { return unread_; } }
        property Windows::UI::Xaml::Visibility UnreadVisibility
        {
            Windows::UI::Xaml::Visibility get() { return unread_->IsEmpty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible; }
        }

    internal:
        TopicEntry(const tg::TopicItem& t);
        int32_t id;
        std::string title;

    private:
        Platform::String^ title_;
        Platform::String^ preview_;
        Platform::String^ unread_;
    };

    // The topics of a forum group (pinned first, as the server sends them). Tapping one opens ChatPage on that
    // topic only ("key\nid\ntitle").
    public ref class TopicsPage sealed
    {
    public:
        TopicsPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    internal:
        std::string chatKey_;
        bool active_;
        Platform::Collections::Vector<TopicEntry^>^ topics_;

        void Load();
        void Topics_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void Refresh_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    };
}
