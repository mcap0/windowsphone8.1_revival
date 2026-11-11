#pragma once

#include "EditorPage.g.h"

namespace Esplora
{
    public ref class EditorPage sealed
    {
    public:
        EditorPage();
        bool HandleHardwareBack();
        // the file another app opened with Esplora: Save also writes it back (when allowed). Set before Navigate.
        static void SetOrigin(Windows::Storage::StorageFile^ file);

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void LoadFile(Platform::String^ path);
        bool Save();
        void GoBack();
        void OnTextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);

        Platform::String^ filePath_;
        Platform::String^ originalText_;
        Platform::String^ lineEnding_;
        int encoding_;
        bool hasBom_;
        bool loading_;
        bool modified_;
        bool readOnly_;
        bool discardRequested_;
        bool confirmed_;
        Windows::Storage::StorageFile^ origin_;
    };
}
