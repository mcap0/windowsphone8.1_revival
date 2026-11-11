#pragma once
#include "MainPage.g.h"
#include "Core/RegText.h"
#include <string>
#include <vector>

namespace Esplora
{
    // Kind: 0 cartella, 1 file, 2 chiave, 3 valore, 4 ramo, 5 pacchetto, 6 risultato ricerca
    [Windows::UI::Xaml::Data::Bindable]
    public ref class Item sealed
    {
    public:
        property Platform::String^ Name;
        property Platform::String^ Glyph;
        property Platform::String^ Details;
        property Platform::String^ Key;
        property int Kind;
        property unsigned long long Size;
    };

    public ref class MainPage sealed
    {
    public:
        MainPage();
        bool HandleHardwareBack();
        void Reload();   // dopo il ritorno da una pagina di modifica

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        // file
        void FileRefresh();
        void FileOpenPath(const std::wstring& path);
        void FileOpenItem(Item^ item, bool asText);
        void FileMenu(Item^ item, Windows::UI::Xaml::FrameworkElement^ anchor);
        void FileLaunch(Item^ item);
        void FileBeginSelect(Item^ item);
        void FileEndSelect();
        std::vector<std::wstring> FileSelectedPaths();
        void FileClipSelected(bool cut);
        void FileDeleteSelected();
        void FileNew(bool folder);
        void FilePaste();
        void FileDelete(Item^ item);
        void FileRename(Item^ item);
        void FileProps(Item^ item);
        // registro
        void RegRefresh();
        void RegOpenPath(const RegText::KeyPath& p);
        void RegItemTapped(Item^ item);
        void RegMenu(Item^ item, Windows::UI::Xaml::FrameworkElement^ anchor);
        void RegNewKey();
        void RegNewValue();
        void RegFind();
        void RegDeleteKey(Item^ item);
        void RegDeleteValue(Item^ item);
        // sistema
        void SysInit();
        void SysRefresh();
        void BuildAppBar();
        void SetFileStatus(const std::wstring& s);
        void SetRegStatus(const std::wstring& s);

        std::wstring filePath_;
        RegText::KeyPath regPath_;
        bool regSearchMode_;
        std::wstring regSearchTitle_;
        bool inited_;
        Windows::UI::Xaml::Controls::CommandBar^ barFiles_;
        Windows::UI::Xaml::Controls::CommandBar^ barReg_;
        Windows::UI::Xaml::Controls::CommandBar^ barSys_;
        Windows::UI::Xaml::Controls::CommandBar^ barSelect_;
    };
}
