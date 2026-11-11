using Posta.Core.Model;

namespace Posta.Services
{
    /// <summary>One-shot hand-off of a message to the compose page.</summary>
    public static class ComposeRequest
    {
        public enum Mode
        {
            New,
            Reply,
            ReplyAll,
            Forward
        }

        public static MailContent ReplyTo { get; private set; }

        /// <summary>Paths chosen in FilePage, picked up when ComposePage comes back.</summary>
        public static System.Collections.Generic.List<string> PendingAttachments { get; set; }
        private static Mode _mode = Mode.New;

        public static void StartNew()
        {
            ReplyTo = null;
            _mode = Mode.New;
        }

        public static void Start(Mode mode, MailContent message)
        {
            _mode = mode;
            ReplyTo = message;
        }

        public static MailContent Take()
        {
            var value = ReplyTo;
            ReplyTo = null;
            return value;
        }

        public static Mode TakeMode()
        {
            var value = _mode;
            _mode = Mode.New;
            return value;
        }
    }
}
