#define NOMINMAX
#include "Watch.h"
#include "Shared.h"
#include "TgPoll.h"

#include <windows.h>
#include <string>

using namespace TelegramTask;
using namespace Platform;
using namespace Windows::ApplicationModel::Background;

// Every 15 minutes (tg-timer) and when the internet comes back (tg-rete), also with the phone locked: one Telegram
// check, with toasts for new messages. Without a network the check waits for tg-rete.
void Watch::Run(IBackgroundTaskInstance^ taskInstance)
{
    BackgroundTaskDeferral^ deferral = taskInstance->GetDeferral();
    SetToastContext(L"watch");
    std::wstring name = taskInstance->Task != nullptr ? taskInstance->Task->Name->Data() : L"?";
    std::wstring network = NetworkText();
    std::wstring line = L"watch (" + name + L"): batteria " + std::to_wstring(BatteryPercent()) + L"%, rete " + network;
    if (network == L"nessuna")
    {
        line += L"; telegram: niente rete, controllo al ritorno della rete";
    }
    else
    {
        std::wstring telegram = PollTelegram();
        SetCheck(telegram);
        line += L"; " + telegram;
    }
    Log(line);
    WaitInbox(4000);
    deferral->Complete();
}
