using System;
using System.IO;
using System.IO.IsolatedStorage;
using System.Threading;
using Microsoft.Phone.Networking.Voip;
using Microsoft.Phone.Scheduler;

// PLAN §69: the three VoIP agents a WP8 VoIP app declares. Here they only write to run.log (isolated storage, shared
// with the app: tools/phonelog 751052db-3f2f-43c6-9428-facae48dda48) so we see when the system runs them.
namespace ProvaVoip.Agents
{
    public static class Log
    {
        static readonly object gate = new object();
        public static void Write(string s)
        {
            lock (gate)
            {
                try
                {
                    using (var store = IsolatedStorageFile.GetUserStoreForApplication())
                    using (var f = new IsolatedStorageFileStream("run.log", FileMode.Append, FileAccess.Write, FileShare.ReadWrite, store))
                    using (var w = new StreamWriter(f))
                        w.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " " + s);
                }
                catch (Exception) { }
            }
        }
    }

    // PLAN §69.7: the MPNS raw push (VoipHttpIncomingCallTask, as Telegram WP 8.0) wakes this agent with the push body.
    // The agent rings the native incoming call itself, as the 2013 VoIP apps did.
    // PLAN §69.8 (1.5.1): calling NotifyComplete right after RequestNewIncomingCall ends the VoIP background process and
    // the ringing call with it (screen on, no ring). As Microsoft's PhoneVoIPApp and Telegram do, the agent stays alive
    // until the incoming call is answered, rejected or ignored, and completes then (guard: 50 s).
    public class ScheduledAgentImpl : ScheduledTaskAgent
    {
        static Windows.Phone.Networking.Voip.VoipPhoneCall ringing;   // referenced while it rings (GC)
        static Timer guard, hangup;
        int finished;

        void Finish(string why)
        {
            if (Interlocked.Exchange(ref finished, 1) != 0) return;
            Log.Write("agente: fine (" + why + "), NotifyComplete");
            ringing = null;
            NotifyComplete();
        }

        protected override void OnInvoke(ScheduledTask task)
        {
            var inc = task as VoipHttpIncomingCallTask;
            if (inc == null)
            {
                Log.Write("agente: " + task.GetType().Name + " " + task.Name);
                NotifyComplete();
                return;
            }
            string body = "";
            try { body = System.Text.Encoding.UTF8.GetString(inc.MessageBody, 0, inc.MessageBody.Length); }
            catch (Exception e) { body = "(" + e.Message + ")"; }
            Log.Write("agente: push MPNS ricevuto, corpo=" + body);
            try
            {
                var coord = Windows.Phone.Networking.Voip.VoipCallCoordinator.GetDefault();
                string dir = Windows.ApplicationModel.Package.Current.InstalledLocation.Path + "\\";
                Windows.Phone.Networking.Voip.VoipPhoneCall call;
                coord.RequestNewIncomingCall("/MainPage.xaml", "Anna Rossi", "VoIP (ponte)",
                    new Uri(dir + "Assets\\ApplicationIcon.png"), "VoIP (ponte)", new Uri(dir + "Assets\\ApplicationIcon.png"),
                    "", new Uri(dir + "Assets\\ring.wav"), Windows.Phone.Networking.Voip.VoipCallMedia.Audio, TimeSpan.FromSeconds(30), out call);
                ringing = call;
                call.AnswerRequested += (s, e) =>
                {
                    Log.Write("agente: AnswerRequested -> NotifyCallActive, fine tra 10 s");
                    s.NotifyCallActive();
                    hangup = new Timer(_ => { s.NotifyCallEnded(); Log.Write("agente: NotifyCallEnded"); Finish("risposta"); }, null, 10000, Timeout.Infinite);
                };
                call.RejectRequested += (s, e) => { Log.Write("agente: RejectRequested " + e.RejectReason); s.NotifyCallEnded(); Finish("rifiuto"); };
                call.EndRequested += (s, e) => { Log.Write("agente: EndRequested"); s.NotifyCallEnded(); Finish("chiusa"); };
                Log.Write("agente: RequestNewIncomingCall ok (dal push), aspetto la risposta");
                guard = new Timer(_ => Finish("50 s senza esito"), null, 50000, Timeout.Infinite);
            }
            catch (Exception e)
            {
                Log.Write("agente: in arrivo 0x" + e.HResult.ToString("X8") + " " + e.Message);
                Finish("errore");
            }
        }
    }

    public class CallInProgressAgentImpl : VoipCallInProgressAgent
    {
        protected override void OnFirstCallStarting() { Log.Write("agente: OnFirstCallStarting"); }
        protected override void OnCancel() { Log.Write("agente: CallInProgress OnCancel"); NotifyComplete(); }
    }

    public class ForegroundLifetimeAgentImpl : VoipForegroundLifetimeAgent
    {
        protected override void OnLaunched() { Log.Write("agente: ForegroundLifetime OnLaunched"); }
        protected override void OnCancel() { Log.Write("agente: ForegroundLifetime OnCancel"); NotifyComplete(); }
    }
}
