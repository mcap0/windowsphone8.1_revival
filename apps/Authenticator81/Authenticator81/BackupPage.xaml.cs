using System;
using System.Collections.Generic;
using System.Linq;
using Authenticator.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.Web.Http;
using Windows.Web.Http.Filters;

namespace Authenticator
{
    public sealed partial class BackupPage : Page
    {
        const string PcImportUrl = "http://192.168.1.148:8765/otp-import.txt";

        public BackupPage() { InitializeComponent(); }

        void OnExport(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            var all = App.Store.Load();
            ExportBox.Text = string.Join("\r\n", all.Select(a => OtpUri.Build(a)));
            StatusText.Text = all.Count + " account esportati.";
        }

        void OnImportText(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            ImportText(ImportBox.Text);
        }

        async void OnImportPc(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            StatusText.Text = "scarico da " + PcImportUrl + " ...";
            try
            {
                // WP8.1 caches GETs unless told otherwise: always fetch the current file.
                var filter = new HttpBaseProtocolFilter();
                filter.CacheControl.ReadBehavior = HttpCacheReadBehavior.MostRecent;
                filter.CacheControl.WriteBehavior = HttpCacheWriteBehavior.NoCache;
                using (var client = new HttpClient(filter))
                using (var response = await client.GetAsync(new Uri(PcImportUrl)))
                {
                    response.EnsureSuccessStatusCode();
                    string text = await response.Content.ReadAsStringAsync();
                    ImportText(text.TrimStart('﻿'));
                }
            }
            catch (Exception ex)
            {
                StatusText.Text = "";
                ErrorText.Text = "Import dal PC non riuscito: " + ex.Message;
            }
        }

        /// <summary>One otpauth:// URI per line; accounts already present (same URI) are skipped.</summary>
        void ImportText(string text)
        {
            var all = App.Store.Load();
            var known = new HashSet<string>(all.Select(a => OtpUri.Build(a)));
            int added = 0, duplicates = 0, invalid = 0;
            string firstError = null;
            foreach (string raw in text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
            {
                string line = raw.Trim();
                if (line.Length == 0) continue;
                try
                {
                    var a = OtpUri.Parse(line);
                    string key = OtpUri.Build(a);
                    if (!known.Add(key)) { duplicates++; continue; }
                    all.Add(a);
                    App.Store.Save(a, all);
                    added++;
                }
                catch (Exception ex)
                {
                    invalid++;
                    if (firstError == null) firstError = ex.Message;
                }
            }
            StatusText.Text = "Importati " + added + ", già presenti " + duplicates + ", non validi " + invalid + ".";
            ErrorText.Text = firstError == null ? "" : "Primo errore: " + firstError;
        }
    }
}
