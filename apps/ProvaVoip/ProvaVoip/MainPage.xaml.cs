using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using Microsoft.Phone.Controls;
using Microsoft.Phone.Networking.Voip;
using Microsoft.Phone.Scheduler;
using Windows.Phone.Networking.Voip;

// PLAN §69 step 0 bis: the WP8 VoIP stack from a Silverlight 8.1 app (ProvaChiamate, a Runtime app, was refused:
// 0x80070057 / 0x800706EF). Native ringing screen, then the keep-alive agent's real period (a candidate wake).
namespace ProvaVoip
{
    public partial class MainPage : PhoneApplicationPage
    {
        VoipPhoneCall call;

        public MainPage()
        {
            InitializeComponent();
            Add("squillo nativo ora", () => Ring());
            Add("squillo nativo tra 15 s (blocca il telefono)", () =>
            {
                Log("squillo tra 15 s");
                var t = new DispatcherTimer { Interval = TimeSpan.FromSeconds(15) };
                t.Tick += (s, e) => { t.Stop(); Ring(); };
                t.Start();
            });
            Add("chiamata in uscita nativa", () => Outgoing());
            Add("agente keep-alive (intervallo minimo)", () => KeepAlive());
            Add("mostra il registro", () => ShowLog());
            Add("push MPNS: canale + sveglia VoIP", () => Push());
            Add("chiudi canale MPNS (poi ricrealo)", () => ClosePush());
            Add("avvia il processo VoIP (agente ForegroundLifetime)", () => LaunchVoip());
            Log("Prova VoIP 1.5.2.0");
            try
            {
                var ch = Microsoft.Phone.Notification.HttpNotificationChannel.Find(channelName);
                if (ch != null) Watch(ch);
            }
            catch (Exception e) { Log("canale all'avvio: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
            ShowLog();
        }

        void Add(string label, Action action)
        {
            var b = new Button { Content = label, HorizontalAlignment = HorizontalAlignment.Stretch };
            b.Click += (s, e) => action();
            Buttons.Children.Add(b);
        }

        void Log(string s)
        {
            Agents.Log.Write(s);
            Dispatcher.BeginInvoke(() => LogText.Text = DateTime.Now.ToString("HH:mm:ss ") + s + "\n" + LogText.Text);
        }

        void ShowLog()
        {
            try
            {
                using (var store = System.IO.IsolatedStorage.IsolatedStorageFile.GetUserStoreForApplication())
                using (var f = store.OpenFile("run.log", System.IO.FileMode.OpenOrCreate, System.IO.FileAccess.Read, System.IO.FileShare.ReadWrite))
                using (var r = new System.IO.StreamReader(f))
                {
                    var all = r.ReadToEnd().Split('\n');
                    var sb = new System.Text.StringBuilder();
                    for (int i = all.Length - 1, n = 0; i >= 0 && n < 40; i--, n++) sb.Append(all[i].TrimEnd('\r')).Append('\n');
                    LogText.Text = sb.ToString();
                }
            }
            catch (Exception e) { LogText.Text = e.Message; }
        }

        void Hook(VoipPhoneCall c)
        {
            c.AnswerRequested += (s, e) =>
            {
                Log("AnswerRequested: attiva 10 s poi fine");
                s.NotifyCallActive();
                Dispatcher.BeginInvoke(() =>
                {
                    var t = new DispatcherTimer { Interval = TimeSpan.FromSeconds(10) };
                    t.Tick += (a, b) => { t.Stop(); s.NotifyCallEnded(); Log("NotifyCallEnded"); };
                    t.Start();
                });
            };
            c.RejectRequested += (s, e) => { Log("RejectRequested " + e.RejectReason); s.NotifyCallEnded(); };
            c.EndRequested += (s, e) => { Log("EndRequested"); s.NotifyCallEnded(); };
            c.HoldRequested += (s, e) => Log("HoldRequested");
            c.ResumeRequested += (s, e) => Log("ResumeRequested");
        }

        void Ring()
        {
            try
            {
                var coord = VoipCallCoordinator.GetDefault();
                string dir = Windows.ApplicationModel.Package.Current.InstalledLocation.Path + "\\";
                coord.RequestNewIncomingCall("/MainPage.xaml", "Anna Rossi", "VoIP",
                    new Uri(dir + "Assets\\ApplicationIcon.png"), "VoIP (ponte)", new Uri(dir + "Assets\\ApplicationIcon.png"),
                    "", new Uri(dir + "Assets\\ring.wav"), VoipCallMedia.Audio, TimeSpan.FromSeconds(30), out call);
                Log("RequestNewIncomingCall ok: suona?");
                Hook(call);
            }
            catch (Exception e) { Log("in arrivo: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
        }

        void Outgoing()
        {
            try
            {
                VoipCallCoordinator.GetDefault().RequestNewOutgoingCall("/MainPage.xaml", "Anna Rossi", "VoIP (ponte)", VoipCallMedia.Audio, out call);
                Log("RequestNewOutgoingCall ok");
                Hook(call);
                call.NotifyCallActive();
                Log("NotifyCallActive: barra della chiamata?");
            }
            catch (Exception e) { Log("in uscita: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
        }

        // PLAN §69.7: the 2013 Telegram path. An MPNS channel (its URI goes to the PC, which POSTs a raw push to it)
        // and a VoipHttpIncomingCallTask on that channel; the push wakes ScheduledAgentImpl, which rings.
        const string channelName = "ProvaVoipChannel";
        const string taskName = "ProvaVoip.IncomingCall";

        bool watched;

        // PLAN §69.8: log raw pushes that reach the app while it is in the foreground (legacy path in WPNCORE) apart
        // from the ones that wake the VoIP agent (agent lines in run.log), to see which path a push took.
        void Watch(Microsoft.Phone.Notification.HttpNotificationChannel ch)
        {
            if (watched) return;
            watched = true;
            ch.ChannelUriUpdated += (s, e) => Log("MPNS URI: " + e.ChannelUri);
            ch.ErrorOccurred += (s, e) => Log("MPNS errore: " + e.ErrorType + " " + e.Message);
            ch.HttpNotificationReceived += (s, e) =>
            {
                string body = "";
                try { using (var r = new System.IO.StreamReader(e.Notification.Body)) body = r.ReadToEnd(); }
                catch (Exception x) { body = "(" + x.Message + ")"; }
                Log("raw ricevuto in primo piano: " + body);
            };
        }

        void Push()
        {
            try
            {
                var ch = Microsoft.Phone.Notification.HttpNotificationChannel.Find(channelName);
                if (ch != null) Watch(ch);
                if (ch == null)
                {
                    ch = new Microsoft.Phone.Notification.HttpNotificationChannel(channelName);
                    Watch(ch);
                    ch.Open();
                    ch.BindToShellToast();
                    Log("canale MPNS creato");
                }
                if (ch.ChannelUri != null) Log("MPNS URI: " + ch.ChannelUri);
                else Log("MPNS URI: in attesa (vedi sopra)");

                var old = ScheduledActionService.Find(taskName);
                if (old != null) ScheduledActionService.Remove(taskName);
                var task = new VoipHttpIncomingCallTask(taskName, channelName) { Description = "Prova VoIP incoming call" };
                ScheduledActionService.Add(task);
                Log("VoipHttpIncomingCallTask registrato, IsScheduled=" + task.IsScheduled);
            }
            catch (Exception e) { Log("push: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
        }

        // PLAN §69.8: a channel that already exists returns its stored URI with no network; closing it makes the next
        // Push() ask the server (our tools/wnsfake once the phone's PushNotifications\Server points to it).
        void ClosePush()
        {
            try
            {
                var ch = Microsoft.Phone.Notification.HttpNotificationChannel.Find(channelName);
                if (ch == null) { Log("nessun canale MPNS da chiudere"); return; }
                ch.Close();
                Log("canale MPNS chiuso");
            }
            catch (Exception e) { Log("chiudi: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
        }

        // PLAN §69.8: the VoIP background process (our ProvaVoip.VoipHost.exe hosting AgHost.BackgroundVoipTask) with
        // no push involved: the ForegroundLifetime agent writes "agente: ForegroundLifetime OnLaunched" if VoIP agents
        // can run at all.
        void LaunchVoip()
        {
            try
            {
                int id;
                VoipBackgroundProcess.Launch(out id);
                Log("VoipBackgroundProcess.Launch ok, processo " + id);
            }
            catch (Exception e) { Log("Launch: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
        }

        void KeepAlive()
        {
            const string name = "keepalive";
            try
            {
                var old = ScheduledActionService.Find(name);
                if (old != null) ScheduledActionService.Remove(name);
                foreach (var minutes in new[] { 1, 5, 15, 30 })
                {
                    try
                    {
                        var t = new VoipKeepAliveTask(name) { Interval = TimeSpan.FromMinutes(minutes) };
                        ScheduledActionService.Add(t);
                        Log("VoipKeepAliveTask ogni " + minutes + " min registrato");
                        return;
                    }
                    catch (Exception e) { Log("keep-alive " + minutes + " min: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
                }
            }
            catch (Exception e) { Log("keep-alive: 0x" + e.HResult.ToString("X8") + " " + e.Message); }
        }
    }
}
