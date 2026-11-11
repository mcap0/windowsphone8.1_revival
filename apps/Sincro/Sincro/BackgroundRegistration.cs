using System;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading.Tasks;
using Windows.ApplicationModel.Background;

namespace Sincro
{
    /// <summary>Registers the 30-minute timer that runs Sincro.Background.SincroTask.</summary>
    public static class BackgroundRegistration
    {
        public const string TaskName = "SincroTimer";
        public const string EntryPoint = "Sincro.Background.SincroTask";

        public static string StatusMessage { get; private set; }

        /// <returns>false when background execution is not allowed.</returns>
        public static async Task<bool> EnsureAsync()
        {
            bool exists = BackgroundTaskRegistration.AllTasks.Values.Any(t => t.Name == TaskName);
            if (exists)
            {
                StatusMessage = "Il timer SincroTimer è già registrato.";
                return true;
            }

            BackgroundAccessStatus access = await BackgroundExecutionManager.RequestAccessAsync().AsTask();
            if (access != BackgroundAccessStatus.AllowedMayUseActiveRealTimeConnectivity)
            {
                StatusMessage = "Registrazione del timer non consentita: " + access + ".";
                return false;
            }

            var builder = new BackgroundTaskBuilder();
            builder.Name = TaskName;
            builder.TaskEntryPoint = EntryPoint;
            builder.SetTrigger(new TimeTrigger(30, false));
            builder.Register();

            StatusMessage = "Timer SincroTimer registrato.";
            return true;
        }
    }
}
