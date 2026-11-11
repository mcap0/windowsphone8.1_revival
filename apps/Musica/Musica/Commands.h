#pragma once

#include "pch.h"

namespace Musica
{
    // musica: protocol used by the Assistente:
    //   musica:riproduci?q=<testo>[&tipo=brano|artista|album|playlist|casuale][&casuale=1]
    //   casuale=1: the request in random order; otherwise in order. tipo=casuale: the whole library at random.
    //   musica:azione?a=pausa|riprendi|avanti|indietro
    // Returns true when something started playing (the caller then shows NowPlayingPage).
    class Commands
    {
    public:
        static bool Run(Windows::Foundation::Uri^ uri);
    };
}
