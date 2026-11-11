// TDLib feasibility test on WP8.1: loads tdjson.dll (built for W10M, with app-local UCRT and VC++ 14 runtime)
// and, on demand, starts a client to see the first updates from Telegram's servers.
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace TdTest
{
    public sealed partial class MainPage : Page
    {
        [DllImport("tdjson.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern IntPtr td_execute(byte[] request);
        [DllImport("tdjson.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern int td_create_client_id();
        [DllImport("tdjson.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern void td_send(int clientId, byte[] request);
        [DllImport("tdjson.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern IntPtr td_receive(double timeout);

        public MainPage()
        {
            InitializeComponent();
            object last;
            if (ApplicationData.Current.LocalSettings.Values.TryGetValue("loading", out last))
                Log("L'app si era chiusa mentre caricava: " + last);
        }

        // load the dependency chain one DLL at a time (native LoadPackagedLibrary), then list what tdjson imports
        // and which of those DLLs or functions the phone lacks
        void Chain_Click(object sender, RoutedEventArgs e)
        {
            string[] order = { "ucrtbase.dll", "api-ms-win-crt-runtime-l1-1-0.dll", "vcruntime140_app.dll",
                "msvcp140_app.dll", "zlib1.dll", "tdjson.dll" };
            var settings = ApplicationData.Current.LocalSettings.Values;
            foreach (string n in order)
            {
                // written before the load: if the process dies, the next start shows where
                settings["loading"] = n;
                try { Log(n + ": " + TdNative.Loader.TryLoad(n)); }
                catch (Exception ex) { Log(n + ": eccezione " + ex.GetType().Name + " " + ex.Message); }
            }
            foreach (string n in new[] { "tdjson.dll", "msvcp140_app.dll", "vcruntime140_app.dll", "ucrtbase.dll" })
            {
                settings["loading"] = "import di " + n;
                try { Log("— import di " + n + "\n" + TdNative.Loader.CheckImports(n)); }
                catch (Exception ex) { Log(n + ": eccezione " + ex.GetType().Name + " " + ex.Message); }
            }
            settings.Remove("loading");
            Send();
        }

        // the log goes to the PC (tools/inbox → share/inbox/tdtest.txt): too long to photograph
        async void Send()
        {
            try
            {
                using (var http = new Windows.Web.Http.HttpClient())
                {
                    await http.PostAsync(new Uri("http://192.168.1.148:8765/tdtest"),
                        new Windows.Web.Http.HttpStringContent(Out.Text));
                }
                Log("(log inviato al PC)");
            }
            catch (Exception ex) { Log("(invio al PC fallito: " + ex.Message + ")"); }
        }

        static byte[] Utf8(string s) { return Encoding.UTF8.GetBytes(s + "\0"); }

        static string FromUtf8(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            int n = 0;
            while (Marshal.ReadByte(p, n) != 0) n++;
            var b = new byte[n];
            Marshal.Copy(p, b, 0, n);
            return Encoding.UTF8.GetString(b, 0, n);
        }

        void Log(string s) { Out.Text += s + "\n"; }

        void Load_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                Log("TDLib: " + TdNative.Loader.TdVersion());
            }
            catch (Exception ex) { Log("ERRORE: " + ex.GetType().Name + " " + ex.Message + " HRESULT 0x" + ex.HResult.ToString("X8")); }
            Send();
        }

        async void Connect_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                td_execute(Utf8("{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}"));
                int id = td_create_client_id();
                string dir = ApplicationData.Current.LocalFolder.Path.Replace("\\", "\\\\");
                td_send(id, Utf8("{\"@type\":\"getOption\",\"name\":\"version\"}"));
                Log("client " + id);
                for (int i = 0; i < 15; i++)
                {
                    string r = await Task.Run(() => FromUtf8(td_receive(1.0)));
                    if (r == null) continue;
                    Log(r.Length > 300 ? r.Substring(0, 300) + "…" : r);
                    if (r.Contains("authorizationStateWaitTdlibParameters"))
                    {
                        // public test credentials are not used: only the parameters check, no login
                        td_send(id, Utf8("{\"@type\":\"setTdlibParameters\",\"database_directory\":\"" + dir + "\\\\td\",\"use_message_database\":false,\"api_id\":1,\"api_hash\":\"x\",\"system_language_code\":\"it\",\"device_model\":\"Lumia 1020\",\"application_version\":\"0.1\"}"));
                    }
                }
            }
            catch (Exception ex) { Log("ERRORE: " + ex.GetType().Name + " " + ex.Message + " HRESULT 0x" + ex.HResult.ToString("X8")); }
            Send();
        }
    }
}
