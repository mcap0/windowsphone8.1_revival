#include "pch.h"
#include "MainPage.xaml.h"
#include "NewChatPage.xaml.h"
#include "ChatPage.xaml.h"
#include "TopicsPage.xaml.h"
#include "SettingsPage.xaml.h"
#include "TgService.h"

using namespace Telegram;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

namespace Telegram
{
    class MainListener : public tg::IClientListener
    {
    public:
        explicit MainListener(MainPage^ page) : page_(page) {}
        virtual void onChatsChanged()
        {
            if (page_ != nullptr)
                page_->RefreshView();
        }
    private:
        MainPage^ page_;
    };
}

MainPage::MainPage() : chats_(ref new Platform::Collections::Vector<ChatVM^>()),
    listener_(nullptr), active_(false)
{
    InitializeComponent();
    ChatList->ItemsSource = chats_;
}

void MainPage::OnNavigatedTo(NavigationEventArgs^)
{
    active_ = true;
    if (listener_ == nullptr)
        listener_ = new MainListener(this);
    TgService::Get().AddListener(listener_);
    UpdateConnectionStatus();
    RefreshView();
    TgService::Get().Client().loadChats([](const tg::RpcErr&) {});
}

void MainPage::OnNavigatedFrom(NavigationEventArgs^)
{
    active_ = false;
    if (listener_ != nullptr)
        TgService::Get().RemoveListener(listener_);
}

void MainPage::UpdateConnectionStatus()
{
    if (active_)
        ConnectionStatus->Text = TgService::Get().online ? L"" : L"Connessione...";
}

void MainPage::RefreshView()
{
    if (!active_)
        return;

    const std::vector<tg::ChatItem>& source = TgService::Get().Client().chats();
    unsigned int count = static_cast<unsigned int>(source.size() > 100 ? 100 : source.size());

    bool sameOrder = chats_->Size == count;
    if (sameOrder)
    {
        for (unsigned int i = 0; i < count; ++i)
        {
            std::string current = FromP(chats_->GetAt(i)->Key);
            if (current != source[i].key)
            {
                sameOrder = false;
                break;
            }
        }
    }

    if (!sameOrder)
    {
        chats_->Clear();
        for (unsigned int i = 0; i < count; ++i)
            chats_->Append(ref new ChatVM(source[i]));
        return;
    }

    for (unsigned int i = 0; i < count; ++i)
        chats_->GetAt(i)->Update(source[i]);
    UpdateConnectionStatus();
}

void MainPage::ChatList_ItemClick(Object^, ItemClickEventArgs^ e)
{
    ChatVM^ chat = dynamic_cast<ChatVM^>(e->ClickedItem);
    if (chat == nullptr)
        return;
    // a group with topics opens on its topic list
    const tg::ChatItem* item = TgService::Get().Client().chat(FromP(chat->Key));
    if (item != nullptr && item->isForum)
        Frame->Navigate(TopicsPage::typeid, chat->Key);
    else
        Frame->Navigate(ChatPage::typeid, chat->Key);
}

void MainPage::Refresh_Click(Object^, RoutedEventArgs^)
{
    TgService::Get().Client().loadChats([](const tg::RpcErr&) {});
}

void MainPage::NewChat_Click(Object^, RoutedEventArgs^)
{
    Frame->Navigate(NewChatPage::typeid);
}

void MainPage::Settings_Click(Object^, RoutedEventArgs^)
{
    Frame->Navigate(SettingsPage::typeid);
}

void MainPage::Chat_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    auto element = dynamic_cast<Windows::UI::Xaml::FrameworkElement^>(sender);
    auto vm = element == nullptr ? nullptr : dynamic_cast<ChatVM^>(element->DataContext);
    if (vm == nullptr)
        return;
    std::string key = FromP(vm->Key);
    const tg::ChatItem* chat = TgService::Get().Client().chat(key);
    if (chat == nullptr)
        return;
    bool muted = chat->muted;
    auto menu = ref new Windows::UI::Xaml::Controls::MenuFlyout();
    if (chat->unread > 0)
    {
        auto read = ref new Windows::UI::Xaml::Controls::MenuFlyoutItem();
        read->Text = L"segna come letto";
        read->Click += ref new Windows::UI::Xaml::RoutedEventHandler([key](Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^)
        {
            const tg::ChatItem* c = TgService::Get().Client().chat(key);
            if (c != nullptr) TgService::Get().Client().markRead(key, c->topId);
        });
        menu->Items->Append(read);
    }
    auto mute = ref new Windows::UI::Xaml::Controls::MenuFlyoutItem();
    mute->Text = muted ? L"riattiva notifiche" : L"silenzia";
    mute->Click += ref new Windows::UI::Xaml::RoutedEventHandler([key, muted](Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^)
    {
        TgService::Get().Client().setMuted(key, !muted, std::function<void(const tg::RpcErr&)>());
    });
    menu->Items->Append(mute);
    menu->ShowAt(element);
}
