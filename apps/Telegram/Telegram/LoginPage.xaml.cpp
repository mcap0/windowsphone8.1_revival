#include "pch.h"
#include "LoginPage.xaml.h"
#include "MainPage.xaml.h"
#include "TgService.h"

using namespace Telegram;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    template<class T>
    auto RpcErrorText(const T& error, int) -> decltype(error.message)
    {
        return error.message;
    }

    template<class T>
    auto RpcErrorText(const T& error, long) -> decltype(error.text)
    {
        return error.text;
    }

    template<class T>
    auto RpcErrorText(const T& error, char) -> decltype(error.error)
    {
        return error.error;
    }

    template<class T>
    std::string RpcErrorText(const T&, ...)
    {
        return "Errore";
    }

    std::string ExtractError(const tg::RpcErr& error)
    {
        return RpcErrorText(error, 0);
    }
}

LoginPage::LoginPage() : busy_(false)
{
    InitializeComponent();
    PhoneBox->TextChanged += ref new Windows::UI::Xaml::Controls::TextChangedEventHandler([](Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
    {
        TgService::Get().Log("login: phone text changed");
    });
    PhoneBox->GotFocus += ref new RoutedEventHandler([](Object^, RoutedEventArgs^) { TgService::Get().Log("login: phone focus"); });
}

void LoginPage::OnNavigatedTo(NavigationEventArgs^)
{
    ErrorText->Text = L"";
    ShowAuthState();
}

void LoginPage::ShowAuthState()
{
    tg::Client::AuthState state = TgService::Get().Client().authState();
    PhoneBox->Visibility = state == tg::Client::WaitPhone || state == tg::Client::AuthUnknown
        ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    CodeBox->Visibility = state == tg::Client::WaitCode ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    PasswordBox->Visibility = state == tg::Client::WaitPassword ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    PasswordHintText->Visibility = state == tg::Client::WaitPassword ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;

    switch (state)
    {
    case tg::Client::WaitCode:
    {
        std::string route = TgService::Get().Client().codeInfo();
        StepText->Text = route == "sms" ? L"Codice inviato via SMS" : L"Codice inviato nell'app Telegram";
        ContinueButton->Content = L"Accedi";
        break;
    }
    case tg::Client::WaitPassword:
        StepText->Text = L"Inserisci la password di verifica in due passaggi";
        PasswordHintText->Text = PStr(TgService::Get().Client().passwordHint());
        ContinueButton->Content = L"Accedi";
        break;
    default:
        StepText->Text = L"Inserisci il tuo numero di telefono";
        ContinueButton->Content = L"Avanti";
        break;
    }
}

void LoginPage::ShowError(const std::string& error)
{
    std::wstring message = Utf16(error);
    if (error == "PHONE_CODE_INVALID")
        message = L"Codice errato";
    else if (error == "PASSWORD_HASH_INVALID")
        message = L"Password errata";
    else if (error.compare(0, 11, "FLOOD_WAIT_") == 0)
    {
        std::string seconds = error.substr(11);
        message = L"Troppi tentativi, riprova tra " + Utf16(seconds) + L" s";
    }
    ErrorText->Text = ref new String(message.c_str());
    busy_ = false;
    ContinueButton->IsEnabled = true;
    ContinueButton->Content = TgService::Get().Client().authState() == tg::Client::WaitPhone
        ? L"Avanti" : L"Accedi";
}

void LoginPage::CompleteLogin()
{
    auto frame = dynamic_cast<Windows::UI::Xaml::Controls::Frame^>(Window::Current->Content);
    if (frame == nullptr)
        return;

    frame->BackStack->Clear();
    frame->Navigate(MainPage::typeid);
}

void LoginPage::Continue_Click(Object^, RoutedEventArgs^)
{
    if (busy_)
        return;

    ErrorText->Text = L"";
    tg::Client& client = TgService::Get().Client();

    if (client.authState() == tg::Client::WaitPhone ||
        client.authState() == tg::Client::AuthUnknown)
    {
        std::string phone = FromP(PhoneBox->Text);
        std::string digits;
        for (size_t i = 0; i < phone.size(); ++i)
            if (phone[i] >= '0' && phone[i] <= '9') digits += phone[i];
        TgService::Get().Log("login: phone digits " + std::to_string(static_cast<long long>(digits.size())));
        if (digits.size() < 9)
        {
            ErrorText->Text = L"Numero incompleto: scrivi il prefisso +39 e il numero";
            return;
        }

        busy_ = true;
        ContinueButton->IsEnabled = false;
        ContinueButton->Content = L"Invio...";
        client.sendCode(phone, [this](const tg::RpcErr& error)
        {
            busy_ = false;
            ContinueButton->IsEnabled = true;
            if (!ExtractError(error).empty())
                ShowError(ExtractError(error));
            else
                ShowAuthState();
        });
        return;
    }

    if (client.authState() == tg::Client::WaitCode)
    {
        std::string code = FromP(CodeBox->Text);
        if (code.empty())
        {
            ErrorText->Text = L"Inserisci il codice ricevuto";
            return;
        }

        busy_ = true;
        ContinueButton->IsEnabled = false;
        ContinueButton->Content = L"Verifica...";
        client.signIn(code, [this](const tg::RpcErr& error)
        {
            if (!ExtractError(error).empty())
            {
                ShowError(ExtractError(error));
                return;
            }

            busy_ = false;
            ContinueButton->IsEnabled = true;
            if (TgService::Get().Client().authState() == tg::Client::Ready || TgService::Get().Engine().authorized())
                CompleteLogin();
            else
                ShowAuthState();
        });
        return;
    }

    if (client.authState() == tg::Client::WaitPassword)
    {
        std::string password = FromP(PasswordBox->Password);
        busy_ = true;
        ContinueButton->IsEnabled = false;
        ContinueButton->Content = L"Verifica...";
        client.checkPassword(password, [this](const tg::RpcErr& error)
        {
            if (!ExtractError(error).empty())
            {
                ShowError(ExtractError(error));
                return;
            }

            busy_ = false;
            ContinueButton->IsEnabled = true;
            if (TgService::Get().Client().authState() == tg::Client::Ready || TgService::Get().Engine().authorized())
                CompleteLogin();
            else
                ShowAuthState();
        });
    }
}
