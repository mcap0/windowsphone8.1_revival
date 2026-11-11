#include "pch.h"
#include <windows.h>
#include <iomanip>
#include "Ui.h"

using namespace Platform;
using namespace concurrency;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

namespace Esplora
{
    std::wstring W(String^ s) { return s == nullptr ? std::wstring() : std::wstring(s->Data(), s->Length()); }
    String^ S(const std::wstring& s) { return ref new String(s.c_str()); }

    static bool StartsWith(const std::wstring& lowerPath, const wchar_t* prefix)
    {
        std::wstring p(prefix);
        return lowerPath.compare(0, p.size(), p) == 0;
    }

    bool IsSafeFilePath(const std::wstring& path)
    {
        std::wstring l = RegText::LowerCopy(path);
        return StartsWith(l, L"c:\\data\\users\\public\\") || StartsWith(l, L"c:\\data\\users\\defapps\\") ||
            StartsWith(l, L"c:\\data\\test\\") || StartsWith(l, L"c:\\test\\") || StartsWith(l, L"d:\\");
    }

    bool IsSafeRegPath(const RegText::KeyPath& p)
    {
        if (p.hive != 1) return false;
        std::wstring l = RegText::LowerCopy(p.sub);
        return l == L"software\\dumbphonetest" || StartsWith(l, L"software\\dumbphonetest\\");
    }

    task<bool> Confirm(String^ title, String^ message, String^ yesText)
    {
        auto dialog = ref new ContentDialog();
        dialog->Title = title;
        auto text = ref new TextBlock();
        text->Text = message;
        text->TextWrapping = TextWrapping::Wrap;
        text->FontSize = 18;
        dialog->Content = text;
        dialog->PrimaryButtonText = yesText;
        dialog->SecondaryButtonText = L"Annulla";
        return create_task(dialog->ShowAsync()).then([](ContentDialogResult r) { return r == ContentDialogResult::Primary; });
    }

    task<bool> Prompt(String^ title, const std::wstring& initial, std::shared_ptr<std::wstring> out, bool multiline)
    {
        auto dialog = ref new ContentDialog();
        dialog->Title = title;
        auto input = ref new TextBox();
        input->Text = S(initial);
        input->AcceptsReturn = multiline;
        input->TextWrapping = multiline ? TextWrapping::Wrap : TextWrapping::NoWrap;
        input->SelectAll();
        dialog->Content = input;
        dialog->PrimaryButtonText = L"OK";
        dialog->SecondaryButtonText = L"Annulla";
        return create_task(dialog->ShowAsync()).then([input, out](ContentDialogResult r)
        {
            if (r != ContentDialogResult::Primary) return false;
            *out = W(input->Text);
            return true;
        });
    }

    void Info(String^ title, String^ message)
    {
        auto dialog = ref new ContentDialog();
        dialog->Title = title;
        auto text = ref new TextBlock();
        text->Text = message;
        text->TextWrapping = TextWrapping::Wrap;
        text->FontSize = 16;
        auto sv = ref new ScrollViewer();
        sv->MaxHeight = 420;
        sv->Content = text;
        dialog->Content = sv;
        dialog->PrimaryButtonText = L"Chiudi";
        create_task(dialog->ShowAsync()).then([](task<ContentDialogResult> t) { try { t.get(); } catch (...) {} });
    }

    std::wstring FormatDate(const FILETIME& time)
    {
        SYSTEMTIME st;
        if (!FileTimeToSystemTime(&time, &st)) return L"data non disponibile";
        std::wstringstream s;
        s << std::setfill(L'0') << std::setw(4) << st.wYear << L'-' << std::setw(2) << st.wMonth << L'-' << std::setw(2) << st.wDay
          << L' ' << std::setw(2) << st.wHour << L':' << std::setw(2) << st.wMinute;
        return s.str();
    }

    void GoBackFrame()
    {
        auto frame = dynamic_cast<Frame^>(Window::Current->Content);
        if (frame != nullptr && frame->CanGoBack) frame->GoBack();
    }
}
