#include "pch.h"
#include "TopicsPage.xaml.h"
#include "ChatPage.xaml.h"
#include "TgService.h"

using namespace Telegram;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

TopicEntry::TopicEntry(const tg::TopicItem& t) : id(t.id), title(t.title)
{
    std::string name = t.title;
    if (t.closed) name += " \xF0\x9F\x94\x92";   // lock
    title_ = PStr(name);
    preview_ = PStr(t.lastText);
    unread_ = t.unread > 0 ? ref new String(std::to_wstring(t.unread).c_str()) : L"";
}

TopicsPage::TopicsPage() : active_(false), topics_(ref new Platform::Collections::Vector<TopicEntry^>())
{
    InitializeComponent();
    Topics->ItemsSource = topics_;
}

void TopicsPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    active_ = true;
    chatKey_ = FromP(dynamic_cast<String^>(e->Parameter));
    const tg::ChatItem* chat = TgService::Get().Client().chat(chatKey_);
    PageTitle->Text = chat == nullptr ? PStr(TgService::Get().Client().titleOf(chatKey_)) : PStr(chat->title);
    Load();
}

void TopicsPage::OnNavigatedFrom(NavigationEventArgs^)
{
    active_ = false;
}

void TopicsPage::Load()
{
    StatusText->Text = topics_->Size == 0 ? L"carico i topic..." : L"";
    std::string key = chatKey_;
    TgService::Get().Client().loadTopics(chatKey_, [this, key](const std::vector<tg::TopicItem>& list, const tg::RpcErr& e)
    {
        if (!active_ || key != chatKey_) return;
        if (!e.ok()) { StatusText->Text = PStr("topic non caricati: " + e.message); return; }
        topics_->Clear();
        for (size_t i = 0; i < list.size(); ++i) topics_->Append(ref new TopicEntry(list[i]));
        StatusText->Text = list.empty() ? L"nessun topic" : L"";
    });
}

void TopicsPage::Topics_ItemClick(Object^, ItemClickEventArgs^ e)
{
    TopicEntry^ t = dynamic_cast<TopicEntry^>(e->ClickedItem);
    if (t == nullptr) return;
    Frame->Navigate(ChatPage::typeid, PStr(chatKey_ + "\n" + std::to_string(static_cast<long long>(t->id)) + "\n" + t->title));
}

void TopicsPage::Refresh_Click(Object^, RoutedEventArgs^)
{
    Load();
}
