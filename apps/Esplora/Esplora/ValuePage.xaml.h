#pragma once
#include "ValuePage.g.h"
#include "Core/RegText.h"

namespace Esplora
{
    public ref class ValuePage sealed
    {
    public:
        ValuePage();
    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
    private:
        void Save();
        void DoSave(const std::vector<unsigned char>& data, uint32_t type, const std::wstring& name);
        void Delete();
        RegText::KeyPath key_;
        std::wstring origName_;
        bool isNew_;
    };
}
