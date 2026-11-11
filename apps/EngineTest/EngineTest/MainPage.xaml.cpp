// Prova motore: loads the engine DLLs built on the PC (engine/out, package folder "engine") and runs their tests on
// a background thread. Results on screen and in share/inbox/enginetest.txt (tools/inbox on the PC).
#include "pch.h"
#include "MainPage.xaml.h"

using namespace EngineTest;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Web::Http;

namespace
{
    typedef int (*InitFunction)(const char* icuDataDirectory);
    typedef int (*OptionsFunction)(char* out, int capacity);
    typedef int (*EvalFunction)(const char* script, char* out, int capacity, double* milliseconds);

    struct Test
    {
        const char* name;
        const char* script;
        const char* expected; // nullptr: benchmark, the result is only shown
    };

    // The first five are share/bench.html, so the numbers compare with MetroSurf and IE11.
    // Non-ASCII text is written as JS escapes; expected results are UTF-8.
    const Test tests[] = {
        { "loop 5M", "(function(){var s=0;for(var i=0;i<5000000;i++){s=(s+i*3)|0;}return s;})()", nullptr },
        { "oggetti 300k", "(function(){var a=[];for(var i=0;i<300000;i++){a.push({x:i,y:i*2});}var s=0;for(var j=0;j<a.length;j++)s+=a[j].y;return s;})()", nullptr },
        { "stringhe 100k", "(function(){var s='';for(var i=0;i<100000;i++){s+=String.fromCharCode(97+i%26);}return s.length;})()", nullptr },
        { "regexp 20k", "(function(){var re=/([a-z]+)@([a-z]+)\\.com/;var n=0;for(var i=0;i<20000;i++){if(re.test('user'+i+'abc@example.com'))n++;}return n;})()", nullptr },
        { "funzioni 2M", "(function(){function f(x){return x+1;}var s=0;for(var i=0;i<2000000;i++)s=f(s);return s;})()", nullptr },
        { "salita a DFG", "(function(){function f(x){return x*2+1;}var s=0;for(var i=0;i<3000000;i++)s+=f(i);return s;})()", "9000000000000" },
        { "arrow/map", "[1,2,3].map(x=>x*2).join()", "2,4,6" },
        { "classi #privati", "class A{#x=41; get x(){return this.#x+1;}} new A().x", "42" },
        { "BigInt", "(2n**64n).toString()", "18446744073709551616" },
        { "JSON", "JSON.stringify(JSON.parse('{\"a\":[1,{\"b\":null}],\"c\":\"\\u00e8\"}'))", "{\"a\":[1,{\"b\":null}],\"c\":\"\xC3\xA8\"}" },
        { "Intl mese (dati ICU)", "new Intl.DateTimeFormat('it-IT',{month:'long'}).format(new Date(2026,9,23))", "ottobre" },
        { "localeCompare it", "['\\u00e9','e','f'].sort(function(a,b){return a.localeCompare(b,'it');}).join()", "e,\xC3\xA9,f" },
        { "regex \\p{L} /u", "/\\p{L}+/u.exec('\\u00e0bc1')[0]", "\xC3\xA0" "bc" },
        { "Date UTC", "new Date(Date.UTC(2026,0,1)).toISOString()", "2026-01-01T00:00:00.000Z" },
        { "Float64Array", "new Float64Array([1.5,2.5]).reduce(function(a,b){return a+b;})", "4" },
        { "Math.sin", "Math.round(Math.sin(1)*1e6)", "841471" },
        { "ricorsione 2000", "(function r(n){return n?1+r(n-1):0;})(2000)", "2000" },
        { "stack overflow", "(function(){try{(function f(){f();})();return 'no';}catch(e){return e instanceof RangeError;}})()", "true" },
        { "generatori", "(function(){function* g(){yield 1;yield 2;}var s=0;for(var v of g())s+=v;return s;})()", "3" },
        { "Proxy", "new Proxy({}, {get:function(t,k){return k+'!';}}).ciao", "ciao!" },
    };

    std::string Utf8(const wchar_t* text)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n : 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, &s[0], n, nullptr, nullptr);
        s.resize(n > 0 ? n - 1 : 0);
        return s;
    }

    std::wstring Wide(const std::string& text)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring s(n > 0 ? n : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &s[0], n);
        s.resize(n > 0 ? n - 1 : 0);
        return s;
    }

    std::string Milliseconds(double ms)
    {
        char text[32];
        sprintf_s(text, "%.1f ms", ms);
        return text;
    }

    // Report to tools/inbox on the PC; lost silently away from home.
    void Post(const std::string& text)
    {
        auto client = ref new HttpClient();
        create_task(client->PostAsync(ref new Uri(L"http://192.168.1.148:8765/enginetest"),
            ref new HttpStringContent(ref new String(Wide(text).c_str())))).then([](task<HttpResponseMessage^> t)
        {
            try { t.get(); } catch (Exception^) { }
        });
    }
}

MainPage::MainPage()
{
    InitializeComponent();
}

void MainPage::OnNavigatedTo(NavigationEventArgs^)
{
    Run();
}

void MainPage::Show(const std::wstring& line)
{
    std::wstring copy = line;
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy]()
    {
        m_log += copy + L"\n";
        Output->Text = ref new String(m_log.c_str());
    }));
}

void MainPage::Run()
{
    std::string engineFolder = Utf8(Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data()) + "\\engine";
    create_task([this, engineFolder]()
    {
        std::string report;
        auto add = [&](const std::string& line) { report += line + "\n"; Show(Wide(line)); };

        HMODULE dll = LoadPackagedLibrary(L"engine\\jsctest.dll", 0);
        if (!dll)
        {
            add("LoadPackagedLibrary(engine\\jsctest.dll) fallita, errore " + std::to_string(GetLastError()));
            Post(report);
            return;
        }
        auto init = reinterpret_cast<InitFunction>(GetProcAddress(dll, "jsctest_init"));
        auto options = reinterpret_cast<OptionsFunction>(GetProcAddress(dll, "jsctest_options"));
        auto eval = reinterpret_cast<EvalFunction>(GetProcAddress(dll, "jsctest_eval"));
        if (!init || !options || !eval)
        {
            add("jsctest.dll senza le funzioni attese");
            Post(report);
            return;
        }

        ULONGLONG start = GetTickCount64();
        int status = init(engineFolder.c_str());
        add("init " + std::to_string(status) + " in " + std::to_string(GetTickCount64() - start) + " ms");
        char buffer[512];
        options(buffer, sizeof buffer);
        add(buffer);

        // Benchmarks three times (the first round includes JIT warm-up), then the correctness checks once.
        for (int round = 1; round <= 3; round++)
        {
            std::string line = "giro " + std::to_string(round) + ":";
            for (const auto& test : tests)
            {
                if (test.expected)
                    continue;
                double ms = 0;
                eval(test.script, buffer, sizeof buffer, &ms);
                line += " " + std::string(test.name) + " " + Milliseconds(ms) + ";";
            }
            add(line);
        }

        int failures = 0;
        for (const auto& test : tests)
        {
            if (!test.expected)
                continue;
            double ms = 0;
            int length = eval(test.script, buffer, sizeof buffer, &ms);
            std::string result(buffer);
            std::string line = std::string(test.name) + ": " + Milliseconds(ms);
            if (length < 0)
            {
                line += " ECCEZIONE " + result;
                failures++;
            }
            else if (test.expected)
            {
                bool ok = result == test.expected;
                line += ok ? " ok" : " SBAGLIATO '" + result + "' invece di '" + test.expected + "'";
                if (!ok)
                    failures++;
            }
            else
                line += " (" + result + ")";
            add(line);
        }
        add(failures ? std::to_string(failures) + " prove fallite" : "tutte le prove ok");
        Post(report);
    });
}
