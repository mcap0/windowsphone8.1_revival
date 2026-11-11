#pragma once
// Processi e servizi (CreateProcessW in kernelbase, Service Control Manager in sechost.dll), risolti a runtime
// come il registro in Root: non sono nella partizione app.
#include <string>

namespace Proc
{
    // run <exe> [argomenti]: avvia il programma; con waitMs > 0 aspetta e riporta il codice di uscita.
    std::wstring Run(const std::wstring& commandLine, unsigned long waitMs);

    // svc query|start|stop|delete <nome>   /   svc create <nome> <percorso exe> [auto|manual]
    std::wstring Service(const std::wstring& args);

    // launch [ex|speech|gen] <uri>: ShellChromeAPI Shell_LaunchSessionByUri (o ...Ex / Shell_LaunchSpeechSessionByUri), come la shell.
    std::wstring Launch(const std::wstring& args);
}
