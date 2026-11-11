#pragma once

#include "MainPage.g.h"

namespace EngineTest
{
    public ref class MainPage sealed
    {
    public:
        MainPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        void Run();
        void Show(const std::wstring& line);

        std::wstring m_log;
    };
}
