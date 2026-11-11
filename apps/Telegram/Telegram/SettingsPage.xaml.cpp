#include "pch.h"
#include "SettingsPage.xaml.h"
#include "LoginPage.xaml.h"
#include "TgService.h"

using namespace Telegram;
using namespace Platform;
using namespace concurrency;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Navigation;

SettingsPage::SettingsPage()
{
    InitializeComponent();
}

void SettingsPage::OnNavigatedTo(NavigationEventArgs^)
{
    tg::Client& client = TgService::Get().Client();
    std::string self = client.selfKey();
    std::wstring account = Utf16(self);
    const std::vector<tg::ChatItem>& chats = client.chats();
    for (size_t i = 0; i < chats.size(); ++i)
    {
        if (chats[i].key == self)
        {
            account = Utf16(chats[i].title);
            break;
        }
    }
    if (account.empty())
        account = L"Account Telegram";
    AccountText->Text = ref new String((L"Account: " + account + L"\n" + Utf16(self)).c_str());
    LogBox->Text = PStr(TgService::Get().LogText());
}

void SettingsPage::SendLog_Click(Object^, RoutedEventArgs^)
{
    TgService::Get().SendLog();
    auto dialog = ref new Windows::UI::Popups::MessageDialog(L"Invio del log avviato.", L"Telegram");
    create_task(dialog->ShowAsync());
}

void SettingsPage::Logout_Click(Object^, RoutedEventArgs^)
{
    auto dialog = ref new Windows::UI::Popups::MessageDialog(
        L"Vuoi davvero uscire dall'account Telegram?", L"Telegram");
    dialog->Commands->Append(ref new Windows::UI::Popups::UICommand(L"Esci"));
    dialog->Commands->Append(ref new Windows::UI::Popups::UICommand(L"Annulla"));
    dialog->DefaultCommandIndex = 1;
    dialog->CancelCommandIndex = 1;
    create_task(dialog->ShowAsync()).then([this](Windows::UI::Popups::IUICommand^ command)
    {
        if (command == nullptr || command->Label != L"Esci")
            return;

        TgService::Get().Client().logOut([this]()
        {
            auto frame = dynamic_cast<Windows::UI::Xaml::Controls::Frame^>(Window::Current->Content);
            if (frame == nullptr)
                return;
            frame->BackStack->Clear();
            frame->Navigate(LoginPage::typeid);
        });
    }, task_continuation_context::use_current());
}
