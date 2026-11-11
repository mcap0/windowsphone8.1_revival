#pragma once

#include "MainPage.g.h"

namespace ProvaChiamate
{
    public ref class MainPage sealed
    {
    public:
        MainPage();

    private:
        void Add(Platform::String^ label, std::function<void()> action);
        void Log(const std::wstring& s);
        void Ring();
        void RunDuplex(int category, int endpoint, int seconds);
        void TryControlChannel();

    };
}
