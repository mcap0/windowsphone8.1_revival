#pragma once

#include <string>

namespace MusicaCore
{
    // How close a spoken request is to a name, 0..1. The Assistente's recognizer is Italian: English titles come back as
    // Italian words ("stai in alai" for Stayin' Alive, "con forta bli nam" for Comfortably Numb), never as the title. So
    // both sides become a sound key (spaces gone, what sounds alike merged) and the keys are compared by edit distance,
    // where a vowel for another vowel costs little: vowels are what an accent changes most.
    std::wstring soundKey(const std::wstring& text);
    double soundSimilarity(const std::wstring& keyA, const std::wstring& keyB);

    // what a name is really called: "Paranoid (2009 Remaster)" -> "Paranoid"
    std::wstring withoutBrackets(const std::wstring& text);

    // the letter of a name in the lists: first letter or digit, accents dropped, digits = '#'
    wchar_t indexLetter(const std::wstring& text);
}
