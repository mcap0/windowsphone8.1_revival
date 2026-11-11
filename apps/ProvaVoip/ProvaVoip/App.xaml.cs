using System.Windows;
using Microsoft.Phone.Controls;

namespace ProvaVoip
{
    public partial class App : Application
    {
        public static PhoneApplicationFrame RootFrame { get; private set; }

        public App()
        {
            InitializeComponent();
            RootFrame = new PhoneApplicationFrame();
            RootVisual = RootFrame;
            UnhandledException += (s, e) => { Agents.Log.Write("ECCEZIONE " + e.ExceptionObject); e.Handled = true; };
        }
    }
}
