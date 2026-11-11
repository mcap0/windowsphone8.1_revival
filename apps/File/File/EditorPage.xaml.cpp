#include "pch.h"
#include "EditorPage.xaml.h"
#include "Core/Fs.h"
#include "Core/Text.h"
#include <windows.ui.xaml.controls.h>

using namespace File;
using namespace Platform;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;

namespace
{
    std::wstring ToWString(String^ value)
    {
        return value == nullptr ? std::wstring() : std::wstring(value->Data(), value->Length());
    }

    String^ ToPlatformString(const std::wstring& value)
    {
        return ref new String(value.c_str());
    }

    String^ ErrorMessage(const std::wstring& error)
    {
        return ToPlatformString(error);
    }
}

EditorPage::EditorPage()
    : encoding_(TextCodec::Utf8),
      hasBom_(false),
      loading_(true),
      modified_(false),
      readOnly_(false),
      discardRequested_(false)
{
    InitializeComponent();

    lineEnding_ = L"\n";
    originalText_ = L"";
    EditorText->TextChanged += ref new TextChangedEventHandler(this, &EditorPage::OnTextChanged);

    auto appBar = ref new AppBar();
    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;

    auto saveButton = ref new AppBarButton();
    saveButton->Label = L"salva";
    saveButton->Icon = ref new SymbolIcon(Symbol::Save);
    saveButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        Save();
    });
    actions->Children->Append(saveButton);

    auto cancelButton = ref new AppBarButton();
    cancelButton->Label = L"annulla";
    cancelButton->Icon = ref new SymbolIcon(Symbol::Cancel);
    cancelButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        modified_ = false;
        GoBack();
    });
    actions->Children->Append(cancelButton);

    appBar->Content = actions;
    BottomAppBar = appBar;
}

void EditorPage::OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e)
{
    Page::OnNavigatedTo(e);
    String^ path = dynamic_cast<String^>(e->Parameter);
    if (path != nullptr)
        LoadFile(path);
}

void EditorPage::LoadFile(String^ path)
{
    loading_ = true;
    filePath_ = path;
    FileNameText->Text = path;

    std::wstring fullPath = ToWString(path);
    size_t slash = fullPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        FileNameText->Text = ToPlatformString(fullPath.substr(slash + 1));

    std::vector<unsigned char> bytes;
    std::wstring error = Fs::ReadFile(fullPath, bytes, 2 * 1024 * 1024);
    if (!error.empty())
    {
        EditorText->Text = L"";
        EditorText->IsEnabled = false;
        StatusText->Text = ToPlatformString(L"Impossibile leggere il file: " + error);
        loading_ = false;
        modified_ = false;
        return;
    }

    TextCodec::Document document;
    if (!TextCodec::Decode(bytes, document, error))
    {
        EditorText->Text = L"";
        EditorText->IsEnabled = false;
        StatusText->Text = ErrorMessage(error);
        loading_ = false;
        modified_ = false;
        return;
    }

    EditorText->IsEnabled = true;
    EditorText->Text = ToPlatformString(document.text);
    originalText_ = EditorText->Text;
    encoding_ = static_cast<int>(document.encoding);
    hasBom_ = document.hasBom;
    lineEnding_ = ToPlatformString(document.lineEnding);

    std::wstring writeError;
    readOnly_ = !Fs::CanWrite(fullPath, writeError);
    if (readOnly_)
    {
        EditorText->IsReadOnly = true;
        StatusText->Text = ToPlatformString(L"Solo lettura: " + writeError);
    }
    else
    {
        EditorText->IsReadOnly = false;
        StatusText->Text = L"";
    }

    modified_ = false;
    loading_ = false;
}

bool EditorPage::Save()
{
    if (readOnly_)
    {
        StatusText->Text = L"File di sola lettura";
        return false;
    }

    TextCodec::Document document;
    document.text = ToWString(EditorText->Text);
    document.encoding = static_cast<TextCodec::Encoding>(encoding_);
    document.hasBom = hasBom_;
    document.lineEnding = ToWString(lineEnding_);

    std::vector<unsigned char> bytes;
    std::wstring error;
    if (!TextCodec::Encode(document, bytes, error))
    {
        StatusText->Text = ToPlatformString(L"Codifica non riuscita: " + error);
        return false;
    }

    error = Fs::WriteFile(ToWString(filePath_), bytes);
    if (!error.empty())
    {
        StatusText->Text = ToPlatformString(L"Salvataggio non riuscito: " + error);
        return false;
    }

    originalText_ = EditorText->Text;
    modified_ = false;
    StatusText->Text = L"Salvato";
    return true;
}

void EditorPage::GoBack()
{
    Windows::UI::Xaml::Controls::Frame^ frame =
        dynamic_cast<Windows::UI::Xaml::Controls::Frame^>(Window::Current->Content);
    if (frame != nullptr && frame->CanGoBack)
        frame->GoBack();
}

bool EditorPage::HandleHardwareBack()
{
    if (!modified_)
        return false;

    discardRequested_ = false;
    auto dialog = ref new ContentDialog();
    dialog->Title = L"Modifiche non salvate";
    dialog->PrimaryButtonText = L"Salva";
    dialog->SecondaryButtonText = L"Resta";

    auto content = ref new StackPanel();
    auto message = ref new TextBlock();
    message->Text = L"Vuoi salvare le modifiche prima di uscire?";
    message->TextWrapping = TextWrapping::Wrap;
    content->Children->Append(message);

    auto discardButton = ref new Button();
    discardButton->Content = L"Scarta modifiche";
    content->Children->Append(discardButton);
    dialog->Content = content;

    discardButton->Click += ref new RoutedEventHandler([this, dialog](Object^, RoutedEventArgs^)
    {
        discardRequested_ = true;
        dialog->Hide();
    });

    create_task(dialog->ShowAsync()).then([this](ContentDialogResult result)
    {
        if (result == ContentDialogResult::Primary)
        {
            if (Save())
                GoBack();
        }
        else if (discardRequested_)
        {
            modified_ = false;
            GoBack();
        }
    });

    return true;
}

void EditorPage::OnTextChanged(Object^, TextChangedEventArgs^)
{
    if (loading_)
        return;

    modified_ = ToWString(EditorText->Text) != ToWString(originalText_);
    if (modified_ && !readOnly_)
        StatusText->Text = L"Modifiche non salvate";
    else if (!readOnly_)
        StatusText->Text = L"";
}
