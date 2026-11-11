// DevBridge: remote test/automation bridge for Windows Phone 8.1 apps (development builds only).
// Link this file into the app csproj (no separate assembly). Call DevBridge.Start(rootFrame)
// from App.OnLaunched after the root Frame exists. Client: tools/phone on the PC.
// HTTP/1.1 on port 8811. Coordinates are WinRT layout units (DIPs), not device pixels.
// Only Windows Phone 8.1 APIs are used.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.Foundation;
using Windows.Graphics.Imaging;
using Windows.Networking;
using Windows.Networking.Connectivity;
using Windows.Networking.Sockets;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.System.Display;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Automation.Peers;
using Windows.UI.Xaml.Automation.Provider;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.Web.Http;
using Windows.Web.Http.Filters;

public static class DevBridge
{
    /// <summary>Set to false before Start() to disable the bridge completely.</summary>
    public static bool Enabled = true;

    public const int Port = 8811;
    public const string PcHost = "192.168.1.148";
    public const int PcPort = 8765;
    public const string CrashKey = "lastCrash";

    /// <summary>App-registered commands for POST /command?name=... (run on the UI thread).</summary>
    public static readonly Dictionary<string, Func<string>> Commands = new Dictionary<string, Func<string>>();

    /// <summary>App-registered handlers for POST /type?...&amp;submit=1 (keyed by element x:Name).</summary>
    public static readonly Dictionary<string, Action> SubmitHandlers = new Dictionary<string, Action>();

    private const int MaxLogLines = 200;
    private static readonly List<string> Lines = new List<string>();
    private static readonly object LogLock = new object();

    private static bool _started;
    private static Frame _rootFrame;
    private static CoreDispatcher _dispatcher;
    private static StreamSocketListener _listener;
    private static DisplayRequest _display;
    private static HttpClient _http;
    private static string _lastAnnounceState;
    private static string _appName = "?";
    private static string _appVersion = "?";

    private sealed class Req
    {
        public string Method = "GET";
        public string Path = "/";
        public readonly Dictionary<string, string> P = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    private sealed class Reply
    {
        public int Status = 200;
        public string Type = "application/json; charset=utf-8";
        public byte[] Body = new byte[0];
    }

    private sealed class Entry
    {
        public string Id;
        public string Kind;
        public string Type;
        public string Name;
        public string Text;
        public bool Enabled;
        public bool Focused;
        public bool HasBounds;
        public double X, Y, W, H;
        public DependencyObject Obj;
    }

    // ---- Public API -------------------------------------------------------------------

    /// <summary>Starts the listener, screen-keep-on and the announce loop. No-op when disabled.</summary>
    public static void Start(Frame rootFrame)
    {
        if (!Enabled || _started || rootFrame == null) return;
        _started = true;
        _rootFrame = rootFrame;
        _dispatcher = Window.Current.Dispatcher;
        try
        {
            _appName = Package.Current.Id.Name;
            PackageVersion v = Package.Current.Id.Version;
            _appVersion = v.Major + "." + v.Minor + "." + v.Build + "." + v.Revision;
        }
        catch (Exception) { }

        Application.Current.UnhandledException += (s, e) =>
            Log("CRASH " + (e.Exception != null ? e.Exception.ToString() : e.Message));

        try
        {
            _display = new DisplayRequest();
            _display.RequestActive();
        }
        catch (Exception ex)
        {
            Log("display request failed: " + ex.Message);
        }

        Log("DevBridge started " + _appName + " " + _appVersion);
        var ignored = ListenAndAnnounceAsync();
    }

    /// <summary>Appends a line to the bridge log (last 200 lines are served by GET /log). Thread-safe.</summary>
    public static void Log(string line)
    {
        string stamped = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture) + " " + line;
        lock (LogLock)
        {
            Lines.Add(stamped);
            if (Lines.Count > MaxLogLines) Lines.RemoveRange(0, Lines.Count - MaxLogLines);
        }
    }

    // ---- Listener and announce ------------------------------------------------------

    private static async Task ListenAndAnnounceAsync()
    {
        try
        {
            _listener = new StreamSocketListener();
            _listener.ConnectionReceived += OnConnection;
            await _listener.BindServiceNameAsync(Port.ToString(CultureInfo.InvariantCulture));
            Log("listening on " + PhoneIp() + ":" + Port);
        }
        catch (Exception ex)
        {
            Log("listener failed: " + ex.Message);
            return;
        }

        while (true)
        {
            await AnnounceAsync();
            await Task.Delay(TimeSpan.FromSeconds(30));
        }
    }

    private static async Task AnnounceAsync()
    {
        string state;
        try
        {
            string url = "http://" + PcHost + ":" + PcPort + "/__devbridge?app=" + Uri.EscapeDataString(_appName) +
                         "&ip=" + PhoneIp() + "&port=" + Port;
            if (_http == null)
            {
                var filter = new HttpBaseProtocolFilter();
                filter.CacheControl.ReadBehavior = HttpCacheReadBehavior.MostRecent;
                filter.CacheControl.WriteBehavior = HttpCacheWriteBehavior.NoCache;
                _http = new HttpClient(filter);
            }
            var request = new HttpRequestMessage(HttpMethod.Get, new Uri(url));
            request.Headers.TryAppendWithoutValidation("Cache-Control", "no-cache");
            using (HttpResponseMessage response = await _http.SendRequestAsync(request))
            {
            }
            state = "ok";
        }
        catch (Exception ex)
        {
            state = "failed: " + ex.Message;
        }
        // Log only when the state changes, so the 30 s loop does not flood the log.
        if (state != _lastAnnounceState)
        {
            _lastAnnounceState = state;
            Log("announce " + state);
        }
    }

    private static string PhoneIp()
    {
        string best = null;
        try
        {
            foreach (HostName h in NetworkInformation.GetHostNames())
            {
                if (h.Type != HostNameType.Ipv4) continue;
                string s = h.CanonicalName;
                if (string.IsNullOrEmpty(s) || s.StartsWith("169.254.", StringComparison.Ordinal) ||
                    s.StartsWith("127.", StringComparison.Ordinal)) continue;
                if (best == null || s.StartsWith("192.168.", StringComparison.Ordinal)) best = s;
            }
        }
        catch (Exception) { }
        return best ?? "unknown";
    }

    // ---- HTTP -----------------------------------------------------------------------

    private static async void OnConnection(StreamSocketListener sender, StreamSocketListenerConnectionReceivedEventArgs args)
    {
        StreamSocket socket = args.Socket;
        try
        {
            Req req = await ReadRequestAsync(socket);
            Reply reply = await HandleAsync(req);
            await SendAsync(socket, reply);
        }
        catch (Exception ex)
        {
            Log("request error: " + ex.Message);
        }
        finally
        {
            socket.Dispose();
        }
    }

    private static async Task<Req> ReadRequestAsync(StreamSocket socket)
    {
        var reader = new DataReader(socket.InputStream);
        reader.InputStreamOptions = InputStreamOptions.Partial;
        var ms = new MemoryStream();
        int headerEnd = -1;
        int contentLength = 0;
        while (true)
        {
            uint n = await reader.LoadAsync(4096);
            if (n == 0) break;
            byte[] chunk = new byte[n];
            reader.ReadBytes(chunk);
            ms.Write(chunk, 0, chunk.Length);
            if (ms.Length > 256 * 1024) throw new InvalidOperationException("request too large");
            byte[] buf = ms.GetBuffer();
            int len = (int)ms.Length;
            if (headerEnd < 0)
            {
                headerEnd = FindHeaderEnd(buf, len);
                if (headerEnd >= 0) contentLength = ContentLengthOf(Encoding.UTF8.GetString(buf, 0, headerEnd));
            }
            if (headerEnd >= 0 && len >= headerEnd + 4 + contentLength) break;
        }
        if (headerEnd < 0) throw new InvalidOperationException("incomplete request");

        byte[] all = ms.ToArray();
        string[] lines = Encoding.UTF8.GetString(all, 0, headerEnd).Split(new[] { "\r\n" }, StringSplitOptions.None);
        string[] parts = lines[0].Split(' ');
        var req = new Req { Method = parts[0].ToUpperInvariant() };
        string target = parts.Length > 1 ? parts[1] : "/";
        int q = target.IndexOf('?');
        string path = q >= 0 ? target.Substring(0, q) : target;
        req.Path = path.Length > 1 ? path.TrimEnd('/') : path;
        if (q >= 0) ParseForm(target.Substring(q + 1), req.P);

        int bodyStart = headerEnd + 4;
        int bodyLen = Math.Min(contentLength, all.Length - bodyStart);
        if (bodyLen > 0) ParseForm(Encoding.UTF8.GetString(all, bodyStart, bodyLen), req.P);
        return req;
    }

    private static int FindHeaderEnd(byte[] buf, int len)
    {
        for (int i = 0; i + 3 < len; i++)
        {
            if (buf[i] == 13 && buf[i + 1] == 10 && buf[i + 2] == 13 && buf[i + 3] == 10) return i;
        }
        return -1;
    }

    private static int ContentLengthOf(string head)
    {
        foreach (string line in head.Split(new[] { "\r\n" }, StringSplitOptions.None))
        {
            if (line.StartsWith("content-length:", StringComparison.OrdinalIgnoreCase))
            {
                int v;
                if (int.TryParse(line.Substring(15).Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out v)) return v;
            }
        }
        return 0;
    }

    private static void ParseForm(string s, Dictionary<string, string> into)
    {
        foreach (string pair in s.Split('&'))
        {
            if (pair.Length == 0) continue;
            int eq = pair.IndexOf('=');
            string k = Dec(eq >= 0 ? pair.Substring(0, eq) : pair);
            string v = eq >= 0 ? Dec(pair.Substring(eq + 1)) : "";
            if (k.Length > 0) into[k] = v;
        }
    }

    private static string Dec(string s)
    {
        try { return Uri.UnescapeDataString(s.Replace('+', ' ')); }
        catch (Exception) { return s; }
    }

    private static async Task SendAsync(StreamSocket socket, Reply reply)
    {
        string head = "HTTP/1.1 " + reply.Status + " " + ReasonOf(reply.Status) + "\r\nContent-Type: " + reply.Type +
                      "\r\nContent-Length: " + reply.Body.Length + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        DataWriter writer = new DataWriter(socket.OutputStream);
        try
        {
            writer.WriteBytes(Encoding.UTF8.GetBytes(head));
            writer.WriteBytes(reply.Body);
            await writer.StoreAsync();
            await writer.FlushAsync();
        }
        finally
        {
            writer.DetachStream();
            writer.Dispose();
        }
    }

    private static string ReasonOf(int status)
    {
        switch (status)
        {
            case 200: return "OK";
            case 404: return "Not Found";
            case 500: return "Internal Server Error";
            default: return "Error";
        }
    }

    private static async Task<Reply> HandleAsync(Req r)
    {
        try
        {
            switch (r.Path)
            {
                case "/ping":
                    return Json(await OnUi(() => PingJson()));
                case "/tree":
                    {
                        bool brief = Flag(r, "brief");
                        return Json(await OnUi(() => TreeJson(brief)));
                    }
                case "/shot":
                    {
                        byte[] png = await OnUiAsync(() => ShotAsync());
                        return new Reply { Type = "image/png", Body = png };
                    }
                case "/tap":
                    return await OnUi(() => Tap(r));
                case "/type":
                    return await OnUi(() => TypeText(r));
                case "/back":
                    return await OnUi(() => Back());
                case "/log":
                    return Json(LogJson());
                case "/log/clear":
                    return await OnUi(() => ClearLog());
                case "/command":
                    {
                        string name = Param(r, "name");
                        return await OnUi(() => RunCommand(name));
                    }
                default:
                    return Json("{\"ok\":false,\"message\":\"unknown endpoint " + Js(r.Path).Trim('"') + "\"}", 404);
            }
        }
        catch (Exception ex)
        {
            return Json("{\"ok\":false,\"message\":" + Js(ex.Message) + "}", 500);
        }
    }

    private static Reply Json(string json, int status = 200)
    {
        return new Reply { Status = status, Type = "application/json; charset=utf-8", Body = Encoding.UTF8.GetBytes(json) };
    }

    private static Reply PlainText(string text, int status = 200)
    {
        return new Reply { Status = status, Type = "text/plain; charset=utf-8", Body = Encoding.UTF8.GetBytes(text) };
    }

    private static Reply Result(bool ok, string message, Entry target = null)
    {
        var sb = new StringBuilder("{\"ok\":").Append(ok ? "true" : "false").Append(",\"message\":").Append(Js(message));
        if (target != null) sb.Append(",\"target\":").Append(EntryJson(target));
        return Json(sb.Append('}').ToString());
    }

    private static string Param(Req r, string key)
    {
        string v;
        return r.P.TryGetValue(key, out v) ? v : null;
    }

    private static bool Flag(Req r, string key)
    {
        string v = Param(r, key);
        return v == "1" || string.Equals(v, "true", StringComparison.OrdinalIgnoreCase);
    }

    // ---- UI thread marshalling --------------------------------------------------------

    private static Task<T> OnUi<T>(Func<T> work)
    {
        var tcs = new TaskCompletionSource<T>();
        var ignored = _dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
        {
            try { tcs.SetResult(work()); }
            catch (Exception ex) { tcs.SetException(ex); }
        });
        return tcs.Task;
    }

    private static Task<T> OnUiAsync<T>(Func<Task<T>> work)
    {
        var tcs = new TaskCompletionSource<T>();
        var ignored = _dispatcher.RunAsync(CoreDispatcherPriority.Normal, async () =>
        {
            try { tcs.SetResult(await work()); }
            catch (Exception ex) { tcs.SetException(ex); }
        });
        return tcs.Task;
    }

    // ---- Endpoints ----------------------------------------------------------------------

    private static string PageName()
    {
        try
        {
            return _rootFrame.SourcePageType.Name ?? "?";
        }
        catch (Exception)
        {
            try { return _rootFrame.Content.GetType().Name; }
            catch (Exception) { return "?"; }
        }
    }

    private static string PingJson()
    {
        return "{\"app\":" + Js(_appName) + ",\"version\":" + Js(_appVersion) + ",\"ip\":" + Js(PhoneIp()) +
               ",\"port\":" + Port + ",\"page\":" + Js(PageName()) +
               ",\"time\":" + Js(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)) +
               ",\"commands\":" + Arr(Commands.Keys) + ",\"submit\":" + Arr(SubmitHandlers.Keys) + "}";
    }

    private static string TreeJson(bool brief)
    {
        List<Entry> all = Collect();
        var sb = new StringBuilder();
        int count = 0;
        sb.Append("{\"app\":").Append(Js(_appName)).Append(",\"page\":").Append(Js(PageName())).Append(",\"elements\":[");
        foreach (Entry e in all)
        {
            if (brief && !Interesting(e)) continue;
            if (count > 0) sb.Append(',');
            sb.Append(EntryJson(e));
            count++;
        }
        sb.Append("],\"count\":").Append(count).Append('}');
        return sb.ToString();
    }

    private static bool Interesting(Entry e)
    {
        return e.Kind == "appbar" || e.Name.Length > 0 || e.Text.Length > 0 || IsActionable(e.Obj);
    }

    private static string EntryJson(Entry e)
    {
        var sb = new StringBuilder();
        sb.Append("{\"id\":").Append(Js(e.Id)).Append(",\"kind\":").Append(Js(e.Kind))
          .Append(",\"type\":").Append(Js(e.Type)).Append(",\"name\":").Append(Js(e.Name))
          .Append(",\"text\":").Append(Js(e.Text))
          .Append(",\"enabled\":").Append(e.Enabled ? "true" : "false")
          .Append(",\"focused\":").Append(e.Focused ? "true" : "false");
        if (e.HasBounds)
        {
            sb.Append(",\"bounds\":{\"x\":").Append(Num(e.X)).Append(",\"y\":").Append(Num(e.Y))
              .Append(",\"w\":").Append(Num(e.W)).Append(",\"h\":").Append(Num(e.H)).Append('}');
        }
        else
        {
            sb.Append(",\"bounds\":null");
        }
        return sb.Append('}').ToString();
    }

    private static string Num(double v)
    {
        return Math.Round(v, 1).ToString(CultureInfo.InvariantCulture);
    }

    private static List<Entry> Collect()
    {
        var list = new List<Entry>();
        var seen = new HashSet<object>();
        UIElement root = Window.Current.Content;
        if (root != null) Walk(root, "0", 0, list, seen);
        Page page = _rootFrame != null ? _rootFrame.Content as Page : null;
        if (page != null)
        {
            AddAppBar(page.TopAppBar, "t", list, seen);
            AddAppBar(page.BottomAppBar, "b", list, seen);
        }
        return list;
    }

    private static void Walk(DependencyObject node, string id, int depth, List<Entry> list, HashSet<object> seen)
    {
        if (node == null || depth > 60 || list.Count >= 2000) return;
        UIElement ui = node as UIElement;
        if (ui != null && ui.Visibility != Visibility.Visible) return;
        FrameworkElement fe = node as FrameworkElement;
        if (fe != null && fe.ActualWidth > 0 && fe.ActualHeight > 0 && seen.Add(fe))
        {
            list.Add(MakeEntry(fe, id, "visual"));
        }
        int n = VisualTreeHelper.GetChildrenCount(node);
        for (int i = 0; i < n; i++)
        {
            Walk(VisualTreeHelper.GetChild(node, i), id + "." + i, depth + 1, list, seen);
        }
    }

    // CommandBar items are not in the visual tree while the bar is closed: listed with kind "appbar".
    private static void AddAppBar(object bar, string tag, List<Entry> list, HashSet<object> seen)
    {
        CommandBar cb = bar as CommandBar;
        if (cb == null) return;
        int i = 0;
        foreach (object o in cb.PrimaryCommands)
        {
            AddBarItem(o, "appbar." + tag + ".p" + i, list, seen);
            i++;
        }
        i = 0;
        foreach (object o in cb.SecondaryCommands)
        {
            AddBarItem(o, "appbar." + tag + ".s" + i, list, seen);
            i++;
        }
    }

    private static void AddBarItem(object o, string id, List<Entry> list, HashSet<object> seen)
    {
        FrameworkElement fe = o as FrameworkElement;
        if (fe == null || !seen.Add(fe)) return;
        list.Add(MakeEntry(fe, id, "appbar"));
    }

    private static Entry MakeEntry(FrameworkElement fe, string id, string kind)
    {
        var e = new Entry
        {
            Id = id,
            Kind = kind,
            Type = fe.GetType().Name,
            Name = fe.Name ?? "",
            Text = TextOf(fe, 0),
            Obj = fe
        };
        Control c = fe as Control;
        e.Enabled = c == null || c.IsEnabled;
        object focus = FocusManager.GetFocusedElement();
        e.Focused = ReferenceEquals(focus, fe);
        if (fe.ActualWidth > 0 && fe.ActualHeight > 0)
        {
            try
            {
                Point p = fe.TransformToVisual(null).TransformPoint(new Point(0, 0));
                e.X = p.X;
                e.Y = p.Y;
                e.W = fe.ActualWidth;
                e.H = fe.ActualHeight;
                e.HasBounds = true;
            }
            catch (Exception) { }
        }
        return e;
    }

    private static string TextOf(DependencyObject o, int depth)
    {
        if (o == null) return "";
        TextBlock tb = o as TextBlock;
        if (tb != null) return tb.Text ?? "";
        TextBox box = o as TextBox;
        if (box != null) return box.Text ?? "";
        AutoSuggestBox asb = o as AutoSuggestBox;
        if (asb != null) return asb.Text ?? "";
        if (o is PasswordBox) return ""; // never expose passwords
        AppBarButton ab = o as AppBarButton;
        if (ab != null) return ab.Label ?? "";
        ContentControl cc = o as ContentControl;
        if (cc != null)
        {
            string s = cc.Content as string;
            if (s != null) return s;
            return FirstTextBlock(o, 0) ?? "";
        }
        return "";
    }

    // Text of the first TextBlock inside a templated control (ListViewItem, Button with a complex content).
    private static string FirstTextBlock(DependencyObject o, int depth)
    {
        if (o == null || depth > 6) return null;
        TextBlock tb = o as TextBlock;
        if (tb != null && !string.IsNullOrEmpty(tb.Text)) return tb.Text;
        int n = VisualTreeHelper.GetChildrenCount(o);
        for (int i = 0; i < n; i++)
        {
            string s = FirstTextBlock(VisualTreeHelper.GetChild(o, i), depth + 1);
            if (!string.IsNullOrEmpty(s)) return s;
        }
        return null;
    }

    private static bool IsActionable(object o)
    {
        return o is ButtonBase || o is ToggleSwitch || o is SelectorItem || o is TextBox ||
               o is PasswordBox || o is AutoSuggestBox;
    }

    private static Entry PickBest(List<Entry> all, Func<Entry, bool> match)
    {
        Entry best = null;
        int bestScore = -1;
        foreach (Entry e in all)
        {
            if (!match(e)) continue;
            int score = (IsActionable(e.Obj) ? 4 : 0) + (e.HasBounds ? 2 : 0) + (e.Enabled ? 1 : 0);
            if (score > bestScore)
            {
                best = e;
                bestScore = score;
            }
        }
        return best;
    }

    private static Reply Tap(Req r)
    {
        List<Entry> all = Collect();
        string id = Param(r, "id");
        string name = Param(r, "name");
        string text = Param(r, "text");
        string xs = Param(r, "x");
        string ys = Param(r, "y");

        if (xs != null && ys != null)
        {
            double x, y;
            if (!double.TryParse(xs, NumberStyles.Float, CultureInfo.InvariantCulture, out x) ||
                !double.TryParse(ys, NumberStyles.Float, CultureInfo.InvariantCulture, out y))
                return Result(false, "bad x or y");
            string msg = null;
            // Visual tree hit test, topmost first.
            foreach (UIElement u in VisualTreeHelper.FindElementsInHostCoordinates(new Point(x, y), Window.Current.Content))
            {
                msg = Activate(u);
                if (msg != null) break;
            }
            // CommandBar buttons are not hit-testable in the visual tree: fall back to their bounds.
            if (msg == null)
            {
                foreach (Entry e in all)
                {
                    if (e.Kind != "appbar" || !e.HasBounds) continue;
                    if (x < e.X || y < e.Y || x > e.X + e.W || y > e.Y + e.H) continue;
                    msg = Activate(e.Obj);
                    if (msg != null) break;
                }
            }
            if (msg == null) return Result(false, "nothing actionable at " + x + "," + y);
            return Result(!msg.StartsWith("error", StringComparison.Ordinal), msg);
        }

        Entry target = null;
        if (id != null)
        {
            target = all.Find(e => e.Id == id);
        }
        else if (name != null)
        {
            target = PickBest(all, e => e.Name == name);
        }
        else if (text != null)
        {
            target = PickBest(all, e => string.Equals(e.Text.Trim(), text.Trim(), StringComparison.OrdinalIgnoreCase));
            if (target == null)
                target = PickBest(all, e => e.Text.IndexOf(text, StringComparison.OrdinalIgnoreCase) >= 0);
        }
        else
        {
            return Result(false, "give text=, name=, id= or x= and y=");
        }
        if (target == null) return Result(false, "no element matches");

        string done = Activate(target.Obj);
        if (done == null) return Result(false, "not actionable: " + target.Type + " has no Button/Toggle/ListViewItem/TextBox in its ancestors", target);
        return Result(!done.StartsWith("error", StringComparison.Ordinal), done, target);
    }

    // Walks up at most 8 ancestors and acts on the first actionable element. Returns null if none.
    private static string Activate(DependencyObject start)
    {
        DependencyObject d = start;
        for (int depth = 0; d != null && depth < 8; depth++)
        {
            string done = ActOn(d);
            if (done != null) return done;
            d = VisualTreeHelper.GetParent(d);
        }
        return null;
    }

    private static string ActOn(DependencyObject d)
    {
        string type = d.GetType().Name;
        if (d is ToggleSwitch || d is ToggleButton)
        {
            IToggleProvider toggle = AutomationOf(d, PatternInterface.Toggle) as IToggleProvider;
            if (toggle != null)
            {
                toggle.Toggle();
                return "toggle " + type;
            }
        }
        if (d is ButtonBase)
        {
            IInvokeProvider inv = AutomationOf(d, PatternInterface.Invoke) as IInvokeProvider;
            if (inv == null) return "error: " + type + " has no Invoke automation pattern";
            inv.Invoke();
            return "invoke " + type;
        }
        if (d is SelectorItem)
        {
            // ItemClick cannot be raised from outside; Invoke/Select are the closest automation actions.
            IInvokeProvider inv = AutomationOf(d, PatternInterface.Invoke) as IInvokeProvider;
            if (inv != null)
            {
                inv.Invoke();
                return "invoke " + type + " (ItemClick may not fire)";
            }
            ISelectionItemProvider sel = AutomationOf(d, PatternInterface.SelectionItem) as ISelectionItemProvider;
            if (sel != null)
            {
                sel.Select();
                return "select " + type + " (ItemClick not raised)";
            }
            return "error: " + type + " has no Invoke/SelectionItem automation pattern";
        }
        if (d is TextBox || d is PasswordBox || d is AutoSuggestBox)
        {
            FrameworkElement fe = (FrameworkElement)d;
            fe.Focus(FocusState.Programmatic);
            return "focus " + type;
        }
        return null;
    }

    private static object AutomationOf(DependencyObject d, PatternInterface pattern)
    {
        FrameworkElement fe = d as FrameworkElement;
        if (fe == null) return null;
        AutomationPeer peer = FrameworkElementAutomationPeer.FromElement(fe);
        return peer == null ? null : peer.GetPattern(pattern);
    }

    private static Reply TypeText(Req r)
    {
        string text = Param(r, "text");
        if (text == null) return Result(false, "missing text=");
        string id = Param(r, "id");
        string name = Param(r, "name");
        if (id == null && name == null) return Result(false, "give name= or id=");

        List<Entry> all = Collect();
        Entry target = id != null ? all.Find(e => e.Id == id) : PickBest(all, e => e.Name == name);
        if (target == null) return Result(false, "element not found");

        string done = SetText(target.Obj, text);
        if (done == null) return Result(false, "not a text box: " + target.Type, target);

        string submit = Param(r, "submit");
        if (!string.IsNullOrEmpty(submit) && submit != "0")
        {
            string key = submit == "1" ? (name ?? target.Name) : submit;
            Action handler;
            if (!string.IsNullOrEmpty(key) && SubmitHandlers.TryGetValue(key, out handler))
            {
                handler();
                done += "; submitted via SubmitHandlers['" + key + "']";
            }
            else
            {
                done += "; no SubmitHandlers entry for '" + key + "'";
            }
        }
        return Result(true, done, target);
    }

    private static string SetText(DependencyObject d, string text)
    {
        TextBox tb = d as TextBox;
        if (tb != null)
        {
            tb.Text = text; // raises TextChanged like user input
            tb.Focus(FocusState.Programmatic);
            tb.SelectionStart = text.Length;
            return "typed into TextBox";
        }
        AutoSuggestBox asb = d as AutoSuggestBox;
        if (asb != null)
        {
            asb.Text = text;
            return "typed into AutoSuggestBox";
        }
        PasswordBox pb = d as PasswordBox;
        if (pb != null)
        {
            pb.Password = text;
            return "typed into PasswordBox";
        }
        return null;
    }

    private static Reply Back()
    {
        if (_rootFrame == null) return Result(false, "no root frame");
        if (!_rootFrame.CanGoBack) return Result(false, "cannot go back: no navigation history");
        _rootFrame.GoBack();
        return Result(true, "went back");
    }

    private static string LogJson()
    {
        var sb = new StringBuilder("{\"lines\":[");
        lock (LogLock)
        {
            for (int i = 0; i < Lines.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append(Js(Lines[i]));
            }
        }
        sb.Append("],\"lastCrash\":").Append(Js(CrashText())).Append('}');
        return sb.ToString();
    }

    private static string CrashText()
    {
        try
        {
            var values = ApplicationData.Current.LocalSettings.Values;
            if (values.ContainsKey(CrashKey) && values[CrashKey] != null) return values[CrashKey].ToString();
        }
        catch (Exception) { }
        return null;
    }

    private static Reply ClearLog()
    {
        lock (LogLock) { Lines.Clear(); }
        try { ApplicationData.Current.LocalSettings.Values.Remove(CrashKey); }
        catch (Exception) { }
        return Result(true, "log cleared");
    }

    private static Reply RunCommand(string name)
    {
        Func<string> f;
        if (name == null || !Commands.TryGetValue(name, out f))
            return PlainText("unknown command '" + name + "'; known: " + Join(Commands.Keys), 404);
        try
        {
            return PlainText(f() ?? "");
        }
        catch (Exception ex)
        {
            return PlainText("error: " + ex, 500);
        }
    }

    private static async Task<byte[]> ShotAsync()
    {
        var rtb = new RenderTargetBitmap();
        await rtb.RenderAsync(Window.Current.Content);
        IBuffer pixels = await rtb.GetPixelsAsync();
        int w = rtb.PixelWidth;
        int h = rtb.PixelHeight;
        byte[] raw = new byte[pixels.Length];
        DataReader.FromBuffer(pixels).ReadBytes(raw);

        var ms = new InMemoryRandomAccessStream();
        BitmapEncoder encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, ms);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)w, (uint)h, 96, 96, raw);
        await encoder.FlushAsync();

        var reader = new DataReader(ms.GetInputStreamAt(0));
        uint n = await reader.LoadAsync((uint)ms.Size);
        byte[] png = new byte[n];
        reader.ReadBytes(png);
        return png;
    }

    // ---- JSON helpers -----------------------------------------------------------------

    private static string Js(string s)
    {
        if (s == null) return "null";
        var sb = new StringBuilder(s.Length + 2);
        sb.Append('"');
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        return sb.Append('"').ToString();
    }

    private static string Arr(IEnumerable<string> items)
    {
        return "[" + Join(items).Replace(", ", ",") + "]" == null ? "[]" : "[" + JoinJson(items) + "]";
    }

    private static string JoinJson(IEnumerable<string> items)
    {
        var sb = new StringBuilder();
        bool first = true;
        foreach (string s in items)
        {
            if (!first) sb.Append(',');
            first = false;
            sb.Append(Js(s));
        }
        return sb.ToString();
    }

    private static string Join(IEnumerable<string> items)
    {
        var sb = new StringBuilder();
        bool first = true;
        foreach (string s in items)
        {
            if (!first) sb.Append(", ");
            first = false;
            sb.Append(s);
        }
        return sb.ToString();
    }
}
