#include "pch.h"
#include "MainPage.xaml.h"
#include "Core/Query.h"

using namespace Risultati;
using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::System;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Web::Http;
using namespace concurrency;

namespace
{
    // Runs in the hidden IE11 page: accepts the EU consent wall, then returns the organic results and the
    // "Panoramica AI" block as JSON. Selectors are loose on purpose (Google changes class names often):
    // a result is a link around an h3/heading, the AI block is the biggest container around its heading
    // that holds no result.
    const wchar_t* ExtractScript = LR"JS((function(){try{
function txt(e){return (e.innerText||e.textContent||'').replace(/[ \t]+\n/g,'\n').trim();}
if(location.hostname.indexOf('consent.')==0){
 var b=document.querySelectorAll('button,input[type=submit],[role=button]');
 for(var i=0;i<b.length;i++){var t=b[i].innerText||b[i].textContent||b[i].value||'';if(/accetta tutto|accept all/i.test(t)){
  var f=b[i];while(f&&f.tagName!='FORM')f=f.parentNode;
  try{b[i].click();}catch(x){}
  if(f)setTimeout(function(){try{f.submit();}catch(x){}},1500);
  return JSON.stringify({consent:1});}}
 return JSON.stringify({consent:0});
}
var sel='a h3, a [role=heading]',res=[],seen={};
function hid(e){var st=e.currentStyle||window.getComputedStyle(e);return st&&(st.display=='none'||st.visibility=='hidden');}
var hs=document.querySelectorAll(sel);
for(var i=0;i<hs.length&&res.length<15;i++){
 var a=hs[i];while(a&&a.tagName!='A')a=a.parentNode;
 if(!a)continue;
 var u=a.href||'';
 // Google wraps results in /url?q=<token>: IE follows its redirect, so the wrapper is kept as is
 if(!/^https?:/.test(u)||(/^https?:\/\/([a-z]+\.)?google\.[a-z.]+\//.test(u)&&!/\/url\?/.test(u))||seen[u])continue;
 var t=txt(hs[i]);if(!t)continue;
 seen[u]=1;
 var c=a;while(c.parentNode&&c.parentNode!=document.body&&c.parentNode.querySelectorAll(sel).length<=1)c=c.parentNode;
 var s='',cite='',ls=txt(c).split('\n');
 for(var k=0;k<ls.length;k++){var l=ls[k].trim();if(!cite&&l.indexOf('›')>=0){cite=l;continue;}if(l==t||/^https?:/.test(l)||l.length<25)continue;if(l.length>s.length)s=l;}
 res.push({t:t,u:u,c:cite,s:s.substring(0,320)});
}
// AI overview as blocks: {l:1 for a bullet, r:[[text,bold],...]}; sources, buttons, hidden lines and the footer are left out
var ai=[],all=document.getElementsByTagName('*');
var BLOCK=/^(DIV|P|UL|OL|LI|H[1-6]|TABLE|TR|TD|SECTION)$/;
function skip(n){return n.nodeType==1&&(/^(SCRIPT|STYLE|svg|SVG|A|BUTTON|IMG|G-IMG)$/.test(n.tagName)||n.getAttribute('role')=='button'||hid(n));}
function hasBlock(n){for(var k=0;k<n.childNodes.length;k++){var ch=n.childNodes[k];if(ch.nodeType==1&&!skip(ch)&&(BLOCK.test(ch.tagName)||hasBlock(ch)))return true;}return false;}
function runs(n,bold,out,noA){for(var k=0;k<n.childNodes.length;k++){var ch=n.childNodes[k];
 if(ch.nodeType==3){var v=ch.nodeValue.replace(/\s+/g,' ');if(v)out.push([v,bold?1:0]);}
 else if(ch.nodeType==1&&!(ch.tagName=='A'&&noA)&&!(ch.tagName!='A'&&skip(ch))&&!hid(ch)){var st=ch.currentStyle||window.getComputedStyle(ch);runs(ch,bold||parseInt(st.fontWeight,10)>=600||/^(B|STRONG)$/.test(ch.tagName),out,noA);}}
 return out;}
function plain(r){var t='';for(var q=0;q<r.length;q++)t+=r[q][0];return t.trim();}
function blocks(n,li){for(var k=0;k<n.childNodes.length;k++){var ch=n.childNodes[k];if(ch.nodeType!=1||skip(ch))continue;
 if(ch.tagName=='LI'||!hasBlock(ch)){var r=runs(ch,false,[]),t=plain(r);
  if(t&&plain(runs(ch,false,[],1))&&!/^(AI Overview|Panoramica AI|Panoramica creata con l'IA|mostra altro|mostra meno|scopri di più)$/i.test(t)&&!/^(Ti interessa|Vuoi sapere|Vuoi approfondire|Desideri|Would you like|Le risposte dell'AI|L'IA pu\u00f2 commettere|AI responses may|Impossibile generare|An AI Overview is not)/i.test(t))ai.push({l:ch.tagName=='LI'?1:0,r:r});}
 else blocks(ch);}}
for(var i=0;i<all.length;i++){
 var e=all[i],ch=e.children||e.childNodes;if(!ch||ch.length>2)continue;
 var tc=(e.textContent||'').trim();
 if(!/^(Panoramica AI|AI Overview|Panoramica creata con l'IA)$/.test(tc))continue;
 var c=e;while(c.parentNode&&c.parentNode!=document.body&&c.parentNode.querySelectorAll(sel).length==0)c=c.parentNode;
 var bs=c.querySelectorAll('[role=button],button');
 for(var k=0;k<bs.length;k++){if(/^mostra altro/i.test(txt(bs[k]))&&!bs[k].__x){bs[k].__x=1;try{bs[k].click();}catch(x){}}}
 blocks(c);break;
}
return JSON.stringify({page:!!(document.body&&document.readyState=='complete'),r:res,ai:ai.slice(0,60)});
}catch(x){return JSON.stringify({err:''+(x&&x.message||x)});}})())JS";

    // IE11 has no Symbol; Google's JS check uses it asynchronously, so a stand-in injected while the page loads
    // lets it finish (strings instead of real symbols, enough for the check).
    const wchar_t* SymbolPolyfill = LR"JS((function(){if(typeof Symbol!='undefined')return 'had';
var n=0,reg={},S=function(d){return '@@sym_'+(d===undefined?'':d)+'_'+(n++)+'_'+Math.random();};
S.iterator='@@iterator';S.asyncIterator='@@asyncIterator';S.toStringTag='@@toStringTag';S.hasInstance='@@hasInstance';
S.toPrimitive='@@toPrimitive';S.species='@@species';S.unscopables='@@unscopables';
S['for']=function(k){return reg[k]||(reg[k]=S(k));};S.keyFor=function(s){for(var k in reg)if(reg[k]===s)return k;};
window.Symbol=S;
var it=function(){var a=this,i=0;return{next:function(){return i<a.length?{value:a[i++],done:false}:{value:void 0,done:true};}};};
try{Object.defineProperty(Array.prototype,'@@iterator',{value:it,writable:true,configurable:true});
Object.defineProperty(String.prototype,'@@iterator',{value:it,writable:true,configurable:true});}catch(e){}
return 'set';})())JS";

    String^ Str(const std::wstring& s) { return ref new String(s.c_str(), (unsigned)s.size()); }

    TextBlock^ Label(String^ text, double size, Color color)
    {
        auto t = ref new TextBlock();
        t->Text = text;
        t->FontSize = size;
        t->Foreground = ref new SolidColorBrush(color);
        t->TextWrapping = TextWrapping::Wrap;
        return t;
    }

    Color Rgb(unsigned char r, unsigned char g, unsigned char b) { return ColorHelper::FromArgb(255, r, g, b); }
}

MainPage::MainPage() : _noResults(false), _aiExpanded(false), _ticks(0), _stable(0), _generation(0), _busy(false), _localStarted(false), _imagesStarted(false)
{
    InitializeComponent();
    Windows::Phone::UI::Input::HardwareButtons::BackPressed +=
        ref new EventHandler<Windows::Phone::UI::Input::BackPressedEventArgs^>(this, &MainPage::OnBackPressed);
    _poll = ref new DispatcherTimer();
    TimeSpan interval;
    interval.Duration = 7000000;  // 0.7 s
    _poll->Interval = interval;
    _poll->Tick += ref new EventHandler<Object^>(this, &MainPage::OnPoll);
    Web->ContentLoading += ref new TypedEventHandler<WebView^, WebViewContentLoadingEventArgs^>([this](WebView^, WebViewContentLoadingEventArgs^) { InjectPolyfill(); });
    Web->DOMContentLoaded += ref new TypedEventHandler<WebView^, WebViewDOMContentLoadedEventArgs^>([this](WebView^, WebViewDOMContentLoadedEventArgs^) { InjectPolyfill(); });
    Web->NavigationStarting += ref new TypedEventHandler<WebView^, WebViewNavigationStartingEventArgs^>(this, &MainPage::Web_NavigationStarting);
    Web->NavigationCompleted += ref new TypedEventHandler<WebView^, WebViewNavigationCompletedEventArgs^>(this, &MainPage::Web_NavigationCompleted);
}

void MainPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    Start(dynamic_cast<String^>(e->Parameter));
}

void MainPage::Start(String^ args)
{
    std::wstring url, shot;
    if (args && RisultatiCore::QueryParam(args->Data(), L"shot", shot) && shot == L"1")
    {
        // debug from the PC (esplora "launch risultati:q=...&shot=1"): screenshot to the inbox, then back to Esplora
        auto timer = ref new DispatcherTimer();
        TimeSpan delay;
        delay.Duration = 30000000;  // 3 s
        timer->Interval = delay;
        timer->Tick += ref new EventHandler<Object^>([this, timer](Object^, Object^) { timer->Stop(); SendShot(); });
        timer->Start();
    }
    std::wstring query = RisultatiCore::QueryFromArgs(args ? args->Data() : L"", url);
    if (!url.empty())
    {
        Open(Str(url));  // a plain link from the Bing page: hand it to IE
        return;
    }
    if (query.empty())
    {
        Finish(L"");
        SearchBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        return;
    }
    Search(query);
}

// Cerca's "<name> contact" suggestion arrives as a plain search: a query that is exactly a contact's name with a
// number goes straight to the dial screen (no Google, no "locale" tab in between); anything else is a web search.
void MainPage::Search(const std::wstring& query)
{
    using namespace Windows::ApplicationModel::Contacts;
    unsigned generation = ++_generation;
    String^ text = Str(query);
    create_task(ContactManager::RequestStoreAsync()).then([text](ContactStore^ store)
    {
        return create_task(store->FindContactsAsync(text));
    }).then([this, generation, query, text](task<Windows::Foundation::Collections::IVectorView<Contact^>^> t)
    {
        if (generation != _generation) return;
        try
        {
            auto list = t.get();
            for (unsigned i = 0; i < list->Size; ++i)
            {
                Contact^ c = list->GetAt(i);
                if (c->Phones->Size == 0 || _wcsicmp(c->DisplayName->Data(), text->Data()) != 0) continue;
                ContactPhone^ phone = c->Phones->GetAt(0);
                for (unsigned k = 0; k < c->Phones->Size; ++k)
                    if (c->Phones->GetAt(k)->Kind == ContactPhoneKind::Mobile) { phone = c->Phones->GetAt(k); break; }
                Log(L"contatto esatto: " + query + L" -> chiamata");
                SetStatus(L"Chiamata a ", c->DisplayName->Data(), L"");
                ShowSearchBarAtBottom(false);
                Windows::ApplicationModel::Calls::PhoneCallManager::ShowPhoneCallUI(phone->Number, c->DisplayName);
                return;
            }
        }
        catch (Exception^ ex)
        {
            Log(L"contatti: " + std::wstring(ex->Message->Data()));
        }
        SearchWeb(query);
    }, task_continuation_context::use_current());
}

void MainPage::SearchWeb(const std::wstring& query)
{
    _query = query;
    _localStarted = false;
    _imagesStarted = false;
    CloseImage();
    _noResults = false;
    _aiExpanded = false;
    ShowSearchBarAtBottom(false);
    _shown.clear();
    // hidden until the first results: its header would cover the "Searching for..." line (it was never shown
    // during the search); the images load meanwhile
    Sections->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Cover->Visibility = Windows::UI::Xaml::Visibility::Visible;
    Sections->SelectedIndex = 0;
    _ticks = _stable = 0;
    ++_generation;
    SearchBox->Text = Str(query);
    Results->Children->Clear();
    ImagesPanel->Children->Clear();
    LocalPanel->Children->Clear();
    Scroller->ScrollToVerticalOffset(0);
    ImagesScroller->ScrollToVerticalOffset(0);
    SetStatus(L"Searching for ", query, L"...");
    ShowProgress(true);
    // pre-accepted EU consent cookie also in the WebView store, so it survives Google's JS-check reload
    try
    {
        auto filter = ref new Windows::Web::Http::Filters::HttpBaseProtocolFilter();
        auto cookie = ref new HttpCookie(L"SOCS", L".google.com", L"/");
        cookie->Value = L"CAESHAgBEhJnd3NfMjAyMzA4MTAtMF9SQzIaAmVuIAEaBgiAo_CmBg";
        cookie->Secure = true;
        filter->CookieManager->SetCookie(cookie);
    }
    catch (Exception^)
    {
    }
    NavigateGoogle(ref new Uri(Str(RisultatiCore::GoogleUrl(query))));
    // images alongside, never ahead of the web results (the page is on its way); "locale" only when opened
    StartImages();
    Log(L"cerca " + query);
    _poll->Start();
}

void MainPage::SearchBox_KeyDown(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key != VirtualKey::Enter) return;
    e->Handled = true;
    std::wstring q = SearchBox->Text->Data();
    if (q.empty()) return;
    Scroller->Focus(Windows::UI::Xaml::FocusState::Programmatic);  // closes the keyboard
    Search(q);
}

// Google answers the in-app IE ("WebView/2.0 ... like Gecko") with a JS check that never completes; the IE
// browser says "like iPhone" and gets the results. So every top-level Google page is requested with the
// browser's User-Agent (and the pre-accepted EU consent cookie): other navigations are cancelled and reissued.
void MainPage::NavigateGoogle(Uri^ uri)
{
    auto request = ref new HttpRequestMessage(HttpMethod::Get, uri);
    request->Headers->Append(L"User-Agent", L"Mozilla/5.0 (Mobile; Windows Phone 8.1; Android 4.0; ARM; Trident/7.0; Touch; rv:11.0; IEMobile/11.0; NOKIA; Lumia 1020) like iPhone OS 7_0_3 Mac OS X AppleWebKit/537 (KHTML, like Gecko) Mobile Safari/537");
    request->Headers->Append(L"Cookie", L"SOCS=CAESHAgBEhJnd3NfMjAyMzA4MTAtMF9SQzIaAmVuIAEaBgiAo_CmBg; CONSENT=YES+");
    _ownNavigation = uri->AbsoluteUri;
    Web->NavigateWithHttpRequestMessage(request);
}

void MainPage::Web_NavigationStarting(WebView^, WebViewNavigationStartingEventArgs^ e)
{
    if (e->Uri == nullptr) return;
    String^ target = e->Uri->AbsoluteUri;
    if (target == _ownNavigation)
    {
        _ownNavigation = nullptr;
        return;
    }
    std::wstring host = e->Uri->Host->Data();
    std::wstring path = e->Uri->Path->Data();
    std::wstring link;
    if (host.find(L"google.") != std::wstring::npos && path == L"/url" &&
        (RisultatiCore::QueryParam(target->Data(), L"q", link) || RisultatiCore::QueryParam(target->Data(), L"url", link)))
    {
        e->Cancel = true;  // Google's redirect wrapper: open the real page in IE
        Open(Str(link));
        return;
    }
    if (host.find(L"google.") == std::wstring::npos && host.find(L"consent.") == std::wstring::npos &&
        (e->Uri->SchemeName == L"http" || e->Uri->SchemeName == L"https"))
    {
        e->Cancel = true;  // a result: IE
        Open(target);
        return;
    }
    if (host.find(L"google.") != std::wstring::npos && host.find(L"consent.") == std::wstring::npos)
    {
        e->Cancel = true;
        Log(L"riemetto " + std::wstring(target->Data()));
        NavigateGoogle(e->Uri);
    }
}

void MainPage::InjectPolyfill()
{
    auto args = ref new Vector<String^>();
    args->Append(ref new String(SymbolPolyfill));
    try
    {
        create_task(Web->InvokeScriptAsync(L"eval", args)).then([this](task<String^> t)
        {
            try { Log(L"polyfill " + std::wstring(t.get()->Data())); } catch (Exception^) {}
        }, task_continuation_context::use_current());
    }
    catch (Exception^)
    {
    }
}

void MainPage::Web_NavigationCompleted(WebView^, WebViewNavigationCompletedEventArgs^ e)
{
    Log(L"pagina " + std::wstring(e->Uri ? e->Uri->AbsoluteUri->Data() : L"?") + (e->IsSuccess ? L" ok" : L" ERRORE"));
    if (!e->IsSuccess && _shown.empty())
    {
        Finish(L"Nessuna risposta: controlla la connessione e premi invio per riprovare");
        return;
    }
    Extract();
}

void MainPage::OnPoll(Object^, Object^)
{
    if (++_ticks > 150)  // ~105 s
    {
        Finish(L"");
        return;
    }
    if (_ticks == 6 && _noResults)
        DumpPage();  // AI block but no links recognised: markup to the PC
    if (_ticks == 143 && _shown.empty())
    {
        // nothing recognised after ~100 s: show Google's own page and send its markup to the PC to fix the extractor
        Sections->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        Cover->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        Status->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        DumpPage();
    }
    Extract();
}

void MainPage::Extract()
{
    if (_busy) return;
    _busy = true;
    unsigned generation = _generation;
    auto args = ref new Vector<String^>();
    args->Append(ref new String(ExtractScript));
    IAsyncOperation<String^>^ op;
    try { op = Web->InvokeScriptAsync(L"eval", args); }
    catch (Exception^ ex) { _busy = false; Log(L"eval: " + std::wstring(ex->Message->Data())); return; }
    create_task(op).then([this, generation](task<String^> t)
    {
        _busy = false;
        if (generation != _generation) return;
        try
        {
            String^ json = t.get();
            std::wstring j = json ? json->Data() : L"(null)";
            if (j != _lastLog) { _lastLog = j; Log(L"tick " + std::to_wstring(_ticks) + L" " + Web->Source->AbsoluteUri->Data() + L"\n" + j.substr(0, 600)); }
            JsonObject^ data;
            if (JsonObject::TryParse(json, &data)) Render(data);
            
        }
        catch (Exception^ ex)
        {
            Log(L"eval errore: " + std::wstring(ex->Message ? ex->Message->Data() : L""));  // page still loading: next tick
        }
    }, task_continuation_context::use_current());
}

void MainPage::Render(JsonObject^ data)
{
    if (data->HasKey(L"consent")) return;  // the consent wall redirects to the results by itself
    if (data->HasKey(L"err"))
    {
        Log(L"estrattore: " + std::wstring(data->GetNamedString(L"err")->Data()));
        return;
    }
    JsonArray^ ai = data->GetNamedArray(L"ai", ref new JsonArray());
    JsonArray^ results = data->GetNamedArray(L"r", ref new JsonArray());
    std::wstring signature = std::wstring(ai->Stringify()->Data()) + L"|" + std::to_wstring(results->Size);
    if (signature == _shown)
    {
        // stop once results are there and nothing changed for ~2 s (the AI block streams in)
        if (results->Size > 0 && ++_stable >= 3) Finish(L"");
        return;
    }
    _stable = 0;
    _noResults = results->Size == 0 && data->GetNamedBoolean(L"page", false);
    if (results->Size == 0 && ai->Size == 0) return;
    _shown = signature;
    Sections->Visibility = Windows::UI::Xaml::Visibility::Visible;
    Results->Children->Clear();
    ShowSearchBarAtBottom(true);

    if (ai->Size > 0) AddOverview(ai);
    for (unsigned i = 0; i < results->Size; ++i)
    {
        JsonObject^ r = results->GetObjectAt(i);
        AddResult(r->GetNamedString(L"t", L""), r->GetNamedString(L"u", L""), r->GetNamedString(L"c", L""), r->GetNamedString(L"s", L""));
    }
    Status->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

// "Panoramica AI": paragraphs, bold section titles and bullets; only the first lines until "mostra altro" is tapped.
void MainPage::AddOverview(JsonArray^ blocks)
{
    auto box = ref new StackPanel();
    box->Margin = Thickness(0, 0, 0, 22);
    auto head = Label(L"Panoramica AI", 24, Rgb(0xFF, 0xFF, 0xFF));
    head->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe WP SemiLight");
    head->Margin = Thickness(0, 0, 0, 4);
    box->Children->Append(head);
    auto more = ref new StackPanel();  // everything after the first lines
    more->Visibility = _aiExpanded ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    size_t shown = 0;
    for (unsigned i = 0; i < blocks->Size; ++i)
    {
        JsonObject^ block = blocks->GetObjectAt(i);
        JsonArray^ runs = block->GetNamedArray(L"r", ref new JsonArray());
        bool bullet = block->GetNamedNumber(L"l", 0) != 0;
        auto t = ref new TextBlock();
        t->TextWrapping = TextWrapping::Wrap;
        t->FontSize = 16;
        t->Foreground = ref new SolidColorBrush(Rgb(0xCC, 0xCC, 0xCC));
        bool allBold = true;
        std::wstring plain;
        for (unsigned k = 0; k < runs->Size; ++k)
        {
            JsonArray^ r = runs->GetArrayAt(k);
            std::wstring text = r->GetStringAt(0)->Data();
            if (k == 0) text.erase(0, text.find_first_not_of(L' ') == std::wstring::npos ? text.size() : text.find_first_not_of(L' '));
            bool bold = r->GetNumberAt(1) != 0;
            allBold = allBold && (bold || text.find_first_not_of(L" .,:;") == std::wstring::npos);
            plain += text;
            auto run = ref new Windows::UI::Xaml::Documents::Run();
            run->Text = Str(text);
            if (bold)
            {
                run->FontWeight = Windows::UI::Text::FontWeights::SemiBold;
                run->Foreground = ref new SolidColorBrush(Rgb(0xFF, 0xFF, 0xFF));
            }
            t->Inlines->Append(run);
        }
        bool nextBullet = i + 1 < blocks->Size && blocks->GetObjectAt(i + 1)->GetNamedNumber(L"l", 0) != 0;
        std::wstring trimmed = plain.substr(0, plain.find_last_not_of(L' ') + 1);
        bool titleShape = !trimmed.empty() && trimmed.size() < 60 && std::wstring(L".:;,?!").find(trimmed.back()) == std::wstring::npos;
        if (!bullet && plain.size() < 80 && (allBold || (titleShape && nextBullet)))
        {
            t->FontSize = 18.667;  // section title
            t->Foreground = ref new SolidColorBrush(Rgb(0xFF, 0xFF, 0xFF));
            t->FontWeight = Windows::UI::Text::FontWeights::SemiBold;
            t->Margin = Thickness(0, 10, 0, 2);
        }
        else if (bullet)
        {
            auto dot = ref new Windows::UI::Xaml::Documents::Run();
            dot->Text = L"\u25AA  ";  // small square bullet
            t->Inlines->InsertAt(0, dot);
            t->Margin = Thickness(12, 2, 0, 2);
        }
        else
            t->Margin = Thickness(0, 2, 0, 4);
        if (shown < 280)
            box->Children->Append(t);
        else
            more->Children->Append(t);
        shown += plain.size();
    }
    box->Children->Append(more);
    if (more->Children->Size > 0 && !_aiExpanded)
    {
        auto link = Label(L"mostra altro", 16, Rgb(0x99, 0x99, 0x99));
        link->Margin = Thickness(0, 4, 0, 0);
        link->Tapped += ref new TappedEventHandler([this, more, link](Object^, TappedRoutedEventArgs^)
        {
            _aiExpanded = true;
            more->Visibility = Windows::UI::Xaml::Visibility::Visible;
            link->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        });
        box->Children->Append(link);
    }
    Results->Children->Append(box);
}

bool MainPage::IsWrapper(String^ url)
{
    std::wstring u = url->Data();
    return u.find(L"google.") != std::wstring::npos && u.find(L"/url?") != std::wstring::npos;
}

// Google's /url?q=<token> wrapper: ask it once (without following) for the real address; cached per wrapper
task<String^> MainPage::Unwrap(String^ url)
{
    std::wstring key = url->Data();
    auto known = _real.find(key);
    if (known != _real.end()) return task_from_result(Str(known->second));
    auto filter = ref new Windows::Web::Http::Filters::HttpBaseProtocolFilter();
    filter->AllowAutoRedirect = false;
    auto client = ref new HttpClient(filter);
    return create_task(client->GetAsync(ref new Uri(url))).then([client](task<HttpResponseMessage^> t)
    {
        String^ real;
        try
        {
            auto response = t.get();
            if (response->Headers->Location != nullptr) real = response->Headers->Location->AbsoluteUri;
            else
            {
                // redirect notice page: the first outside link
                std::wstring body = create_task(response->Content->ReadAsStringAsync()).get()->Data();
                size_t p = body.find(L"href=\"http");
                while (p != std::wstring::npos)
                {
                    size_t e = body.find(L'"', p + 6);
                    std::wstring link = body.substr(p + 6, e - p - 6);
                    for (size_t amp; (amp = link.find(L"&amp;")) != std::wstring::npos;) link.replace(amp, 5, L"&");
                    if (link.find(L"google.") == std::wstring::npos) { real = Str(link); break; }
                    p = body.find(L"href=\"http", e);
                }
            }
        }
        catch (Exception^)
        {
        }
        return real;
    }, task_continuation_context::use_arbitrary()).then([this, key](String^ real)
    {
        if (real != nullptr && !real->IsEmpty()) _real[key] = real->Data();
        return real;
    }, task_continuation_context::use_current());
}

void MainPage::AddResult(String^ title, String^ url, String^ cite, String^ snippet)
{
    auto item = ref new StackPanel();
    item->Margin = Thickness(0, 0, 0, 20);
    item->Background = ref new SolidColorBrush(Rgb(0, 0, 0));  // whole area tappable
    auto t = Label(title, 24, Rgb(0xFF, 0xFF, 0xFF));
    t->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe WP SemiLight");
    t->MaxLines = 2;
    item->Children->Append(t);
    bool wrapped = cite->IsEmpty() && IsWrapper(url);
    auto green = Label(cite->IsEmpty() && !wrapped ? Str(RisultatiCore::ShortUrl(url->Data())) : cite, 16, Rgb(0x33, 0x99, 0x33));
    item->Children->Append(green);
    if (wrapped)  // the green line shows the site, not google.com/url
    {
        green->Text = L"\u2026";
        Unwrap(url).then([green](String^ real)
        {
            green->Text = real != nullptr && !real->IsEmpty() ? Str(RisultatiCore::ShortUrl(real->Data())) : L"google.com/url";
        }, task_continuation_context::use_current());
    }
    if (!snippet->IsEmpty())
    {
        auto s = Label(snippet, 16, Rgb(0xCC, 0xCC, 0xCC));
        s->MaxLines = 3;
        s->Margin = Thickness(0, 2, 0, 0);
        item->Children->Append(s);
    }
    item->Tapped += ref new TappedEventHandler([this, url](Object^, TappedRoutedEventArgs^)
    {
        auto known = _real.find(url->Data());  // already resolved: straight to the site
        Open(known != _real.end() ? Str(known->second) : url);
    });
    item->Holding += ref new HoldingEventHandler([this, item, url](Object^, HoldingRoutedEventArgs^ e)
    {
        if (e->HoldingState == Windows::UI::Input::HoldingState::Started) ShowLink(item, url);
    });
    Results->Children->Append(item);
}

void MainPage::Open(String^ url)
{
    try
    {
        Launcher::LaunchUriAsync(ref new Uri(url));  // http(s) -> IE
    }
    catch (Exception^)
    {
    }
}

// WP8.1 apps have no clipboard API: show the link selected in a text box, the system copy button does the rest.
void MainPage::ShowLink(FrameworkElement^ target, String^ url)
{
    auto panel = ref new StackPanel();
    panel->Children->Append(Label(L"copia link: tocca il testo selezionato", 18.667, Rgb(0x99, 0x99, 0x99)));
    auto box = ref new TextBox();
    box->Text = url;
    box->IsReadOnly = true;
    box->TextWrapping = TextWrapping::Wrap;
    box->FontSize = 20;
    box->Margin = Thickness(0, 8, 0, 8);
    panel->Children->Append(box);
    auto open = ref new Button();
    open->Content = ref new String(L"apri");
    open->Click += ref new RoutedEventHandler([this, url](Object^, RoutedEventArgs^) { Open(url); });
    panel->Children->Append(open);
    auto flyout = ref new Flyout();
    flyout->Content = panel;
    flyout->Opened += ref new EventHandler<Object^>([box](Object^, Object^)
    {
        box->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        box->SelectAll();
    });
    flyout->ShowAt(target);
    if (!IsWrapper(url)) return;
    Unwrap(url).then([box](String^ real)
    {
        if (real == nullptr || real->IsEmpty()) return;
        box->Text = real;
        box->SelectAll();
    }, task_continuation_context::use_current());
}

// Top: same place as the Bing "Searching for..." page; bottom: under the results, as in the old Bing app.
void MainPage::ShowSearchBarAtBottom(bool bottom)
{
    Grid::SetRow(SearchBar, bottom ? 3 : 0);
    SearchBar->Margin = bottom ? Thickness(19, 6, 19.65, 6) : Thickness(19, 3, 19.65, 0);
}

// Same look as the OEM "Searching for prova..." screen: grey lead-in, white query.
void MainPage::SetStatus(const std::wstring& lead, const std::wstring& query, const std::wstring& tail)
{
    Status->Visibility = Windows::UI::Xaml::Visibility::Visible;
    Status->Inlines->Clear();
    auto add = [this](const std::wstring& text, Color color)
    {
        if (text.empty()) return;
        auto run = ref new Windows::UI::Xaml::Documents::Run();
        run->Text = Str(text);
        run->Foreground = ref new SolidColorBrush(color);
        Status->Inlines->Append(run);
    };
    add(lead, Rgb(0x99, 0x99, 0x99));
    add(query, Rgb(0xFF, 0xFF, 0xFF));
    add(tail, Rgb(0xFF, 0xFF, 0xFF));
}

// System tray "Searching..." with the moving dots, as in the Bing app.
void MainPage::ShowProgress(bool on)
{
    try
    {
        auto bar = Windows::UI::ViewManagement::StatusBar::GetForCurrentView()->ProgressIndicator;
        if (on)
        {
            bar->Text = L"Searching...";
            bar->ProgressValue = nullptr;
            bar->ShowAsync();
        }
        else
            bar->HideAsync();
    }
    catch (Exception^)
    {
    }
}

void MainPage::Mic_Tapped(Object^, TappedRoutedEventArgs^)
{
    Launcher::LaunchUriAsync(ref new Uri(L"assistente:tasto"));
}

// No results recognised: send the page markup to the PC to adapt the selectors.
void MainPage::DumpPage()
{
    auto args = ref new Vector<String^>();
    args->Append(L"document.documentElement.outerHTML");
    try
    {
        create_task(Web->InvokeScriptAsync(L"eval", args)).then([this](task<String^> t)
        {
            try { Log(L"HTML\n" + std::wstring(t.get()->Data())); } catch (Exception^) {}
        }, task_continuation_context::use_current());
    }
    catch (Exception^)
    {
    }
}

// Our page as PNG (base64) to share/inbox/risultati-shot.txt, to line it up with the Bing page pixel by pixel.
void MainPage::SendShot()
{
    auto rtb = ref new Windows::UI::Xaml::Media::Imaging::RenderTargetBitmap();
    auto stream = ref new Windows::Storage::Streams::InMemoryRandomAccessStream();
    create_task(rtb->RenderAsync(Root)).then([rtb]()
    {
        return rtb->GetPixelsAsync();
    }, task_continuation_context::use_current()).then([rtb, stream](Windows::Storage::Streams::IBuffer^ pixels)
    {
        return create_task(Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId, stream))
            .then([rtb, pixels](Windows::Graphics::Imaging::BitmapEncoder^ enc)
        {
            Array<unsigned char>^ bytes;
            Windows::Security::Cryptography::CryptographicBuffer::CopyToByteArray(pixels, &bytes);
            enc->SetPixelData(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,
                (unsigned)rtb->PixelWidth, (unsigned)rtb->PixelHeight, 96, 96, bytes);
            return enc->FlushAsync();
        });
    }).then([stream]()
    {
        auto reader = ref new Windows::Storage::Streams::DataReader(stream->GetInputStreamAt(0));
        return create_task(reader->LoadAsync((unsigned)stream->Size)).then([reader](unsigned n) { return reader->ReadBuffer(n); });
    }).then([](task<Windows::Storage::Streams::IBuffer^> t)
    {
        String^ text;
        try { text = Windows::Security::Cryptography::CryptographicBuffer::EncodeToBase64String(t.get()); }
        catch (Exception^ ex) { text = L"ERRORE " + ex->Message; }
        auto client = ref new HttpClient();
        return create_task(client->PostAsync(ref new Uri(L"http://192.168.1.148:8765/risultati-shot"), ref new HttpStringContent(text)));
    }).then([](task<HttpResponseMessage^> t)
    {
        try { t.get(); } catch (Exception^) {}
        Launcher::LaunchUriAsync(ref new Uri(L"esplora:"));
    }, task_continuation_context::use_current());
}

// Diagnostics to tools/inbox on the PC (share/inbox/risultati.txt); silently lost away from home.
void MainPage::Log(const std::wstring& text)
{
    try
    {
        auto client = ref new HttpClient();
        create_task(client->PostAsync(ref new Uri(L"http://192.168.1.148:8765/risultati"), ref new HttpStringContent(Str(text + L"\n"))))
            .then([client](task<HttpResponseMessage^> t) { try { t.get(); } catch (Exception^) {} });
    }
    catch (Exception^)
    {
    }
}

void MainPage::Finish(String^ message)
{
    _poll->Stop();
    ShowProgress(false);
    if (!message->IsEmpty()) SetStatus(message->Data(), L"", L"");
}

void MainPage::Sections_SelectionChanged(Object^, SelectionChangedEventArgs^)
{
    CloseImage();
    if (Sections->SelectedIndex == 1) StartImages();
    if (Sections->SelectedIndex == 2 && !_localStarted && !_query.empty())
    {
        _localStarted = true;
        SearchLocal();
    }
}

void MainPage::StartImages()
{
    if (_imagesStarted || _query.empty()) return;
    _imagesStarted = true;
    LoadImages();
}

// "immagini": Bing image search (thumbnails in a grid; tap = full size with "salva").
void MainPage::LoadImages()
{
    unsigned generation = _generation;
    auto note = Label(L"cerco le immagini\u2026", 18.667, Rgb(0x99, 0x99, 0x99));
    ImagesPanel->Children->Append(note);
    auto client = ref new HttpClient();
    auto request = ref new HttpRequestMessage(HttpMethod::Get, ref new Uri(Str(RisultatiCore::BingImagesUrl(_query))));
    // the phone's IE gets a page that only opens the old Bing app; the desktop page has the image data
    request->Headers->Append(L"User-Agent", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36");
    typedef std::shared_ptr<std::vector<std::wstring>> Tiles;
    create_task(client->SendRequestAsync(request)).then([client](HttpResponseMessage^ response)
    {
        return response->Content->ReadAsStringAsync();
    }).then([](String^ page)
    {
        // the ~300 KB page is read off the UI thread, so the web results never wait for it
        return std::make_shared<std::vector<std::wstring>>(RisultatiCore::AttributeValues(page->Data(), L"m", 60));
    }, task_continuation_context::use_arbitrary()).then([this, generation, note](task<Tiles> t)
    {
        if (generation != _generation) return;
        Tiles tiles;
        try { tiles = t.get(); }
        catch (Exception^ ex)
        {
            note->Text = L"immagini non disponibili: " + ex->Message;
            return;
        }
        double side = std::floor((Windows::UI::Xaml::Window::Current->Bounds.Width - 34) / 3);
        if (side < 60) side = 140;
        auto grid = ref new VariableSizedWrapGrid();
        grid->Orientation = Orientation::Horizontal;
        grid->ItemWidth = side;
        grid->ItemHeight = side;
        int count = 0;
        for (const std::wstring& m : *tiles)
        {
            JsonObject^ o;
            if (!JsonObject::TryParse(Str(m), &o)) continue;
            String^ thumb = o->GetNamedString(L"turl", L"");
            String^ full = o->GetNamedString(L"murl", L"");
            if (thumb->IsEmpty() || full->IsEmpty()) continue;
            String^ page = o->GetNamedString(L"purl", L"");
            String^ title = o->GetNamedString(L"t", L"");
            auto bmp = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage();
            bmp->DecodePixelWidth = (int)side;
            bmp->UriSource = ref new Uri(thumb);
            auto img = ref new Image();
            img->Source = bmp;
            img->Stretch = Stretch::UniformToFill;
            img->Margin = Thickness(0, 0, 4, 4);
            img->Tapped += ref new TappedEventHandler([this, full, page, title](Object^, TappedRoutedEventArgs^) { ShowImage(full, page, title); });
            grid->Children->Append(img);
            ++count;
        }
        if (count == 0)
        {
            note->Text = L"nessuna immagine";
            Log(L"immagini: niente nella pagina (" + std::to_wstring(tiles->size()) + L" riquadri)");
            return;
        }
        note->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        ImagesPanel->Children->Append(grid);
    }, task_continuation_context::use_current());
}

void MainPage::ShowImage(String^ full, String^ page, String^ title)
{
    CloseImage();
    auto viewer = ref new Grid();
    viewer->Background = ref new SolidColorBrush(Rgb(0, 0, 0));
    Grid::SetRowSpan(viewer, 4);
    auto rows = viewer->RowDefinitions;
    auto r0 = ref new RowDefinition();
    r0->Height = GridLength(1, GridUnitType::Star);
    rows->Append(r0);
    auto r1 = ref new RowDefinition();
    r1->Height = GridLength::Auto;
    rows->Append(r1);

    auto ring = ref new ProgressRing();
    ring->IsActive = true;
    viewer->Children->Append(ring);
    auto bmp = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage();
    auto img = ref new Image();
    img->Stretch = Stretch::Uniform;
    img->Margin = Thickness(0, 30, 0, 0);
    bmp->ImageOpened += ref new RoutedEventHandler([ring](Object^, RoutedEventArgs^) { ring->IsActive = false; });
    bmp->ImageFailed += ref new ExceptionRoutedEventHandler([ring, viewer, this](Object^, ExceptionRoutedEventArgs^)
    {
        ring->IsActive = false;
        auto t = Label(L"immagine non disponibile, prova \"pagina\"", 20, Rgb(0x99, 0x99, 0x99));
        t->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        t->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        viewer->Children->Append(t);
    });
    bmp->UriSource = ref new Uri(full);
    img->Source = bmp;
    viewer->Children->Append(img);

    auto bottom = ref new StackPanel();
    bottom->Margin = Thickness(19, 6, 19, 12);
    Grid::SetRow(bottom, 1);
    auto caption = Label(title, 16, Rgb(0xCC, 0xCC, 0xCC));
    caption->MaxLines = 2;
    bottom->Children->Append(caption);
    auto buttons = ref new StackPanel();
    buttons->Orientation = Orientation::Horizontal;
    auto save = ref new Button();
    save->Content = L"salva";
    save->Click += ref new RoutedEventHandler([full, save](Object^, RoutedEventArgs^)
    {
        save->IsEnabled = false;
        save->Content = L"salvo\u2026";
        auto client = ref new HttpClient();
        create_task(client->GetBufferAsync(ref new Uri(full))).then([client, full](Windows::Storage::Streams::IBuffer^ data)
        {
            // name from the address, with an image extension (Foto shows only those)
            std::wstring u = full->Data();
            size_t q = u.find_first_of(L"?#");
            if (q != std::wstring::npos) u = u.substr(0, q);
            std::wstring name = u.substr(u.find_last_of(L'/') + 1);
            for (wchar_t& c : name) if (!iswalnum(c) && c != L'.' && c != L'-' && c != L'_') c = L'_';
            std::wstring lower = name;
            for (wchar_t& c : lower) c = towlower(c);
            bool image = false;
            for (const wchar_t* ext : { L".jpg", L".jpeg", L".png", L".gif", L".bmp" })
                if (lower.size() > wcslen(ext) && lower.compare(lower.size() - wcslen(ext), wcslen(ext), ext) == 0) image = true;
            if (name.empty() || name.size() > 80) name = L"immagine";
            if (!image) name += L".jpg";
            return create_task(Windows::Storage::KnownFolders::SavedPictures->CreateFileAsync(Str(name),
                Windows::Storage::CreationCollisionOption::GenerateUniqueName)).then([data](Windows::Storage::StorageFile^ file)
            {
                return Windows::Storage::FileIO::WriteBufferAsync(file, data);
            });
        }).then([save](task<void> t)
        {
            try
            {
                t.get();
                save->Content = L"salvata";
            }
            catch (Exception^)
            {
                save->Content = L"riprova";
                save->IsEnabled = true;
            }
        }, task_continuation_context::use_current());
    });
    buttons->Children->Append(save);
    if (!page->IsEmpty())
    {
        auto open = ref new Button();
        open->Content = L"pagina";
        open->Margin = Thickness(10, 0, 0, 0);
        open->Click += ref new RoutedEventHandler([this, page](Object^, RoutedEventArgs^) { Open(page); });
        buttons->Children->Append(open);
    }
    auto close = ref new Button();
    close->Content = L"chiudi";
    close->Margin = Thickness(10, 0, 0, 0);
    close->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { CloseImage(); });
    buttons->Children->Append(close);
    bottom->Children->Append(buttons);
    viewer->Children->Append(bottom);
    Root->Children->Append(viewer);
    _viewer = viewer;
}

void MainPage::CloseImage()
{
    if (_viewer == nullptr) return;
    unsigned index;
    if (Root->Children->IndexOf(_viewer, &index)) Root->Children->RemoveAt(index);
    _viewer = nullptr;
}

void MainPage::OnBackPressed(Object^, Windows::Phone::UI::Input::BackPressedEventArgs^ e)
{
    if (_viewer == nullptr) return;
    CloseImage();
    e->Handled = true;
}

// "locale": files on the phone (pictures, music, videos libraries) whose name has every word of the search.
// Folder by folder (a deep query on a whole library never finished), rows added as they are found; stops after
// 600 folders, 60 files or 25 s.
void MainPage::SearchLocal()
{
    unsigned generation = _generation;
    auto note = Label(L"cerco nel telefono\u2026", 18.667, Rgb(0x99, 0x99, 0x99));
    LocalPanel->Children->Append(note);
    auto words = std::make_shared<std::vector<std::wstring>>();
    {
        std::wstring w;
        for (wchar_t c : _query + L" ")
        {
            if (iswspace(c)) { if (!w.empty()) words->push_back(w); w.clear(); }
            else w += towlower(c);
        }
    }
    AddContacts(generation);
    auto queue = std::make_shared<std::vector<Windows::Storage::StorageFolder^>>();
    try { queue->push_back(Windows::Storage::KnownFolders::VideosLibrary); } catch (Exception^) {}
    try { queue->push_back(Windows::Storage::KnownFolders::MusicLibrary); } catch (Exception^) {}
    try { queue->push_back(Windows::Storage::KnownFolders::PicturesLibrary); } catch (Exception^) {}
    auto found = std::make_shared<int>(0);
    auto visited = std::make_shared<int>(0);
    ULONGLONG deadline = GetTickCount64() + 25000;
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, generation, note, words, queue, found, visited, deadline, step]()
    {
        if (generation != _generation) return;
        if (queue->empty() || *found >= 60 || *visited >= 600 || GetTickCount64() > deadline)
        {
            if (*found == 0) note->Text = L"nessun file nel telefono con questo nome (foto, musica, video)";
            else note->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            return;
        }
        auto folder = queue->back();
        queue->pop_back();
        ++*visited;
        create_task(folder->GetItemsAsync()).then([this, generation, note, words, queue, found, step]
            (task<Windows::Foundation::Collections::IVectorView<Windows::Storage::IStorageItem^>^> t)
        {
            if (generation != _generation) return;
            try
            {
                for (auto item : t.get())
                {
                    auto sub = dynamic_cast<Windows::Storage::StorageFolder^>(item);
                    if (sub != nullptr) { queue->push_back(sub); continue; }
                    auto file = dynamic_cast<Windows::Storage::StorageFile^>(item);
                    if (file == nullptr || *found >= 60) continue;
                    std::wstring name = file->Name->Data();
                    for (wchar_t& c : name) c = towlower(c);
                    bool all = true;
                    for (const std::wstring& w : *words) if (name.find(w) == std::wstring::npos) { all = false; break; }
                    if (!all) continue;
                    ++*found;
                    auto row = ref new StackPanel();
                    row->Margin = Thickness(0, 0, 0, 16);
                    row->Background = ref new SolidColorBrush(Rgb(0, 0, 0));  // whole area tappable
                    auto title = Label(file->Name, 24, Rgb(0xFF, 0xFF, 0xFF));
                    title->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe WP SemiLight");
                    title->MaxLines = 2;
                    row->Children->Append(title);
                    row->Children->Append(Label(file->DisplayType, 16, Rgb(0x33, 0x99, 0x33)));
                    row->Tapped += ref new TappedEventHandler([file](Object^, TappedRoutedEventArgs^) { Launcher::LaunchFileAsync(file); });
                    LocalPanel->Children->Append(row);
                }
            }
            catch (Exception^) {}
            (*step)();
        }, task_continuation_context::use_current());
    };
    (*step)();
}

// Contacts matching the search, on top of "locale": tap = call (mobile number first).
void MainPage::AddContacts(unsigned generation)
{
    using namespace Windows::ApplicationModel::Contacts;
    String^ query = Str(_query);
    create_task(ContactManager::RequestStoreAsync()).then([query](ContactStore^ store)
    {
        return create_task(store->FindContactsAsync(query));
    }).then([this, generation](task<Windows::Foundation::Collections::IVectorView<Contact^>^> t)
    {
        if (generation != _generation) return;
        Windows::Foundation::Collections::IVectorView<Contact^>^ list;
        try { list = t.get(); }
        catch (Exception^ ex)
        {
            Log(L"contatti: " + std::wstring(ex->Message->Data()));
            return;
        }
        unsigned at = 0;
        for (unsigned i = 0; i < list->Size && at < 15; ++i)
        {
            Contact^ c = list->GetAt(i);
            if (c->Phones->Size == 0) continue;
            ContactPhone^ phone = c->Phones->GetAt(0);
            for (unsigned k = 0; k < c->Phones->Size; ++k)
                if (c->Phones->GetAt(k)->Kind == ContactPhoneKind::Mobile) { phone = c->Phones->GetAt(k); break; }
            auto row = ref new StackPanel();
            row->Margin = Thickness(0, 0, 0, 16);
            row->Background = ref new SolidColorBrush(Rgb(0, 0, 0));  // whole area tappable
            auto name = Label(c->DisplayName, 24, Rgb(0xFF, 0xFF, 0xFF));
            name->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Segoe WP SemiLight");
            row->Children->Append(name);
            row->Children->Append(Label(L"contatto \u00B7 " + phone->Number, 16, Rgb(0x33, 0x99, 0x33)));
            String^ number = phone->Number;
            String^ display = c->DisplayName;
            row->Tapped += ref new TappedEventHandler([number, display](Object^, TappedRoutedEventArgs^)
            {
                Windows::ApplicationModel::Calls::PhoneCallManager::ShowPhoneCallUI(number, display);
            });
            LocalPanel->Children->InsertAt(at++, row);  // above the files
        }
    }, task_continuation_context::use_current());
}

