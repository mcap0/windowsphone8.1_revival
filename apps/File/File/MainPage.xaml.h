#pragma once

#include "MainPage.g.h"

#include <string>
#include <vector>

namespace File
{
    ref class FileItem sealed
    {
    public:
        property Platform::String^ Name;
        property Platform::String^ Glyph;
        property Platform::String^ Details;
        property Platform::String^ FullPath;
        property Platform::String^ Modified;
        property Platform::String^ Attributes;
        property bool IsDir;
        property unsigned long long Size;
    };

    public ref class MainPage sealed
    {
    public:
        MainPage();
        bool HandleHardwareBack();
        void SaveView();

    private:
        void NavigateTo(Platform::String^ path, bool setBoundary);
        void Refresh();
        void GoUp();
        void OpenItem(FileItem^ item, bool forceText);
        void ShowContextMenu(FileItem^ item);
        void CopySelection(bool cut);
        void DeletePaths(const std::vector<std::wstring>& paths);
        void PasteClipboard();
        void PromptForName(bool createFolder);
        void SetStatus(Platform::String^ text);
        void PathBox_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OnItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnListRightTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::RightTappedRoutedEventArgs^ e);
        void OnSelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);

        Platform::String^ currentPath_;
        Platform::String^ placeRoot_;
        FileItem^ contextItem_;
        bool selecting_;
        bool suppressSelection_;
    };
}
