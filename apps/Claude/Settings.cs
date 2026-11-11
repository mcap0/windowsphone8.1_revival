using System;
using System.Linq;
using System.Threading.Tasks;
using Claude.Core;
using Windows.Security.Credentials;
using Windows.Storage;

namespace Claude
{
    /// <summary>The API key (in the credential locker) and the other settings.</summary>
    public static class Settings
    {
        const string VaultResource = "Claude API";
        const string VaultUser = "api-key";
        const string ModelKey = "model";
        const string WebSearchKey = "web_search";
        const string SystemKey = "system";
        const string HistoryFile = "chats.json";

        public const string DefaultSystem =
            "Rispondi in italiano, in modo conciso. L'utente legge da un vecchio telefono con uno schermo piccolo: " +
            "niente tabelle, pochi elenchi, testo semplice senza Markdown complesso.";

        static ApplicationDataContainer Values => ApplicationData.Current.LocalSettings;

        public static string ApiKey
        {
            get
            {
                try
                {
                    var credential = new PasswordVault().Retrieve(VaultResource, VaultUser);
                    credential.RetrievePassword();
                    return credential.Password;
                }
                catch (Exception) { return null; } // not saved yet
            }
            set
            {
                var vault = new PasswordVault();
                try { foreach (var c in vault.FindAllByResource(VaultResource)) vault.Remove(c); } catch (Exception) { }
                if (!string.IsNullOrWhiteSpace(value)) vault.Add(new PasswordCredential(VaultResource, VaultUser, value.Trim()));
            }
        }

        public static string Model
        {
            get => Values.Values[ModelKey] as string ?? ClaudeModel.DefaultId;
            set => Values.Values[ModelKey] = value;
        }

        public static bool WebSearch
        {
            get => Values.Values[WebSearchKey] as bool? ?? true;
            set => Values.Values[WebSearchKey] = value;
        }

        public static string SystemPrompt
        {
            get => Values.Values[SystemKey] as string ?? DefaultSystem;
            set => Values.Values[SystemKey] = value;
        }

        static ChatHistory history;

        public static async Task<ChatHistory> LoadHistoryAsync()
        {
            if (history != null) return history;
            var item = await ApplicationData.Current.LocalFolder.TryGetItemAsync(HistoryFile);
            history = ChatHistory.Parse(item is StorageFile f ? await FileIO.ReadTextAsync(f) : null);
            return history;
        }

        public static async Task SaveHistoryAsync()
        {
            if (history == null) return;
            var folder = ApplicationData.Current.LocalFolder;
            // write to a temporary file first, so that a crash never leaves a truncated file
            var tmp = await folder.CreateFileAsync(HistoryFile + ".tmp", CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteTextAsync(tmp, history.ToJson());
            await tmp.RenameAsync(HistoryFile, NameCollisionOption.ReplaceExisting);
        }
    }
}
