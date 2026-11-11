#include "pch.h"
#include "NewChatPage.xaml.h"
#include "ChatPage.xaml.h"
#include "TgService.h"

#include <set>

using namespace Telegram;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    std::string Lower(const std::string& s)
    {
        std::wstring w = Utf16(s);
        for (size_t i = 0; i < w.size(); ++i)
            w[i] = towlower(w[i]);
        return Utf8(w);
    }

    std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, s.find_last_not_of(" \t") - a + 1);
    }

    String^ DetailOf(const tg::ContactItem& c)
    {
        std::string d;
        if (!c.username.empty())
            d = "@" + c.username;
        if (!c.phone.empty())
            d += (d.empty() ? "+" : "  +") + c.phone;
        if (d.empty())
            d = c.key[0] == 'h' ? "canale o gruppo" : c.key[0] == 'c' ? "gruppo" : "";
        return PStr(d);
    }
}

NewChatPage::NewChatPage() : active_(false), forward_(false)
{
    InitializeComponent();
    timer_ = ref new DispatcherTimer();
    Windows::Foundation::TimeSpan pause;
    pause.Duration = 7000000; // 0.7 s
    timer_->Interval = pause;
    timer_->Tick += ref new Windows::Foundation::EventHandler<Object^>([this](Object^, Object^)
    {
        timer_->Stop();
        SearchNow();
    });
}

void NewChatPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    active_ = true;
    String^ mode = dynamic_cast<String^>(e->Parameter);
    forward_ = mode != nullptr && mode == L"inoltra";
    PageTitle->Text = forward_ ? L"inoltra a" : L"nuova chat";
    found_.clear();
    foundFor_.clear();
    Query->Text = L"";
    StatusText->Text = L"Caricamento contatti...";
    TgService::Get().Client().loadContacts([this](const std::vector<tg::ContactItem>& list, const tg::RpcErr& e)
    {
        if (!active_)
            return;
        contacts_ = list;
        if (!e.ok())
            StatusText->Text = PStr("Errore: " + e.message);
        Show();
    });
}

void NewChatPage::OnNavigatedFrom(NavigationEventArgs^)
{
    active_ = false;
    timer_->Stop();
}

void NewChatPage::Query_TextChanged(Object^, TextChangedEventArgs^)
{
    Show();
    timer_->Stop();
    if (Trim(FromP(Query->Text)).size() >= 3)
        timer_->Start();
}

void NewChatPage::Query_KeyUp(Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter)
        return;
    timer_->Stop();
    SearchNow();
}

void NewChatPage::SearchNow()
{
    std::string q = Trim(FromP(Query->Text));
    if (q.size() < 2 || q == foundFor_)
        return;
    StatusText->Text = L"Ricerca su Telegram...";
    TgService::Get().Client().searchPeople(q, [this, q](const std::vector<tg::ContactItem>& list, const tg::RpcErr& e)
    {
        if (!active_ || Trim(FromP(Query->Text)) != q)
            return;
        found_ = list;
        foundFor_ = q;
        Show();
        if (!e.ok())
            StatusText->Text = PStr("Errore: " + e.message);
    });
}

void NewChatPage::Show()
{
    std::string q = Lower(Trim(FromP(Query->Text)));
    if (!q.empty() && q[0] == '@')
        q = q.substr(1);
    auto items = ref new Platform::Collections::Vector<PersonEntry^>();
    std::set<std::string> shown;
    if (forward_)
    {
        const std::vector<tg::ChatItem>& chats = TgService::Get().Client().chats();
        for (size_t i = 0; i < chats.size(); ++i)
        {
            if (!q.empty() && Lower(chats[i].title).find(q) == std::string::npos)
                continue;
            if (shown.insert(chats[i].key).second)
                items->Append(ref new PersonEntry(PStr(chats[i].key), PStr(chats[i].title),
                                                  PStr(chats[i].isChannel ? "canale" : chats[i].isGroup ? "gruppo" : "chat")));
        }
    }
    for (size_t i = 0; i < contacts_.size(); ++i)
    {
        const tg::ContactItem& c = contacts_[i];
        if (!q.empty() && Lower(c.name).find(q) == std::string::npos && Lower(c.username).find(q) == std::string::npos &&
            c.phone.find(q) == std::string::npos)
            continue;
        if (!shown.insert(c.key).second)
            continue;
        items->Append(ref new PersonEntry(PStr(c.key), PStr(c.name), DetailOf(c)));
    }
    if (!q.empty() && Lower(foundFor_).find(q) != std::string::npos)
        for (size_t i = 0; i < found_.size(); ++i)
            if (shown.insert(found_[i].key).second)
                items->Append(ref new PersonEntry(PStr(found_[i].key), PStr(found_[i].name), DetailOf(found_[i])));
    People->ItemsSource = items;
    StatusText->Text = items->Size == 0 ? (q.size() < 3 ? L"Nessun contatto" : L"Nessun risultato")
                                        : ref new String((std::to_wstring(items->Size) + L" risultati").c_str());
}

void NewChatPage::People_ItemClick(Object^, ItemClickEventArgs^ e)
{
    auto person = dynamic_cast<PersonEntry^>(e->ClickedItem);
    if (person == nullptr)
        return;
    if (forward_)
    {
        TgService& svc = TgService::Get();
        if (svc.pendingForwardIds.empty())
            return;
        std::string to = FromP(person->Key);
        std::vector<int32_t> ids;
        ids.swap(svc.pendingForwardIds);
        StatusText->Text = L"Inoltro...";
        svc.Client().forwardMessages(svc.pendingForwardFrom, ids, to, [](const tg::MsgItem&, const tg::RpcErr& err)
        {
            if (!err.ok())
                TgService::Get().Log("forward: " + err.message);
        });
    }
    Frame->Navigate(ChatPage::typeid, person->Key);
    // back from the chat goes to the chat list, not to this page
    auto stack = Frame->BackStack;
    if (stack->Size > 0 && stack->GetAt(stack->Size - 1)->SourcePageType.Name == NewChatPage::typeid->FullName)
        stack->RemoveAt(stack->Size - 1);
}
