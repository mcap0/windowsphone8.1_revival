using System;

namespace Sincro.Logic
{
    /// <summary>
    /// RemoteId values stored on phone contacts. The GUID prefix is fixed for this app,
    /// so contacts created by other apps (or the People app) never match our prefix.
    /// </summary>
    public static class RemoteIds
    {
        public const string Prefix = "sincro:7c1e4a52-9d0b-4f6e-8a33-2b5d6e1f0a94:";

        public static string For(string resourceName)
        {
            if (string.IsNullOrEmpty(resourceName)) throw new ArgumentException("resourceName");
            return Prefix + resourceName;
        }

        public static bool IsOwn(string remoteId)
        {
            return remoteId != null && remoteId.StartsWith(Prefix, StringComparison.Ordinal);
        }

        /// <summary>Returns the Google resource name for one of our RemoteIds, or null.</summary>
        public static string ResourceNameOf(string remoteId)
        {
            return IsOwn(remoteId) ? remoteId.Substring(Prefix.Length) : null;
        }
    }
}
