#pragma once

#include "ImagePage.g.h"
#include <string>

namespace Telegram
{
    public ref class ImagePage sealed
    {
    public:
        ImagePage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        std::string mediaKey_;
        std::string bytes_;
        void Save_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Scroller_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
        void SetProgress(int64_t received, int64_t total);
    };
}
