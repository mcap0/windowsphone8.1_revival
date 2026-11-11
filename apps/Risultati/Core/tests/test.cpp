// g++ -std=c++17 -I.. test.cpp -o /tmp/t && /tmp/t
#include "../Query.h"
#include <cassert>
#include <cstdio>
using namespace RisultatiCore;

int main()
{
    std::wstring url;
    assert(QueryFromArgs(L"http://192.168.1.148:8765/search?q=perletv&input=1&form=MB1024", url) == L"perletv" && url.empty());
    assert(QueryFromArgs(L"https://www.bing.com/search?q=caff%C3%A8+al+bar&form=X", url) == L"caff\u00e8 al bar");
    assert(QueryFromArgs(L"https://www.bing.com/search?form=X&q=a%26b", url) == L"a&b");
    assert(QueryFromArgs(L"https://example.com/page", url) == L"" && url == L"https://example.com/page");
    assert(QueryFromArgs(L"risultati:q=ciao+mondo", url) == L"ciao mondo");
    assert(QueryFromArgs(L"risultati:?q=x", url) == L"x");
    assert(QueryFromArgs(L"risultati:ciao%20mondo", url) == L"ciao mondo");
    assert(QueryFromArgs(L"prova", url) == L"prova");
    assert(QueryFromArgs(L"", url) == L"");
    assert(UrlEncode(L"caff\u00e8 & co") == L"caff%C3%A8+%26+co");
    assert(GoogleUrl(L"a b") == L"https://www.google.com/search?hl=it&q=a+b");
    assert(ShortUrl(L"https://www.example.com/a/b/?x=1") == L"example.com/a/b");
    assert(HtmlUnescape(L"{&quot;murl&quot;:&quot;a&amp;b&quot;} &#39;x&#x27;") == L"{\"murl\":\"a&b\"} 'x'");
    assert(HtmlUnescape(L"a & b &unknown; c") == L"a & b &unknown; c");
    auto m = AttributeValues(L"<a class=\"iusc\" m=\"{&quot;t&quot;:1}\" x=\"\"></a><a m=\"2\"/><b nom=\"3\">", L"m", 10);
    assert(m.size() == 2 && m[0] == L"{\"t\":1}" && m[1] == L"2");
    assert(BingImagesUrl(L"etna vino") == L"https://www.bing.com/images/search?setlang=it&q=etna+vino");
    puts("ok");
}
