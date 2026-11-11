// 
// This is the source code of Telegram for Windows Phone v. 3.x.x.
// It is licensed under GNU GPL v. 2 or later.
// You should have received a copy of the license in this archive (see LICENSE).
// 
// Copyright Evgeny Nadymov, 2013-present.
// 
namespace Telegram.Api
{
    /// <summary>
    /// Telegram application credentials (api_id / api_hash from my.telegram.org).
    /// No values are compiled in: the app fills them at startup from the credentials
    /// the user entered (stored in ApplicationData / PasswordVault), before any login.
    /// </summary>
    public static class ApiConfig
    {
        public static int ApiId { get; set; }

        public static string ApiHash { get; set; }

        public static bool IsConfigured
        {
            get { return ApiId > 0 && !string.IsNullOrEmpty(ApiHash); }
        }
    }
}
