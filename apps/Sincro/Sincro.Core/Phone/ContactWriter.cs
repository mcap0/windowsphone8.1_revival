using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Sincro.Logic;
using Windows.Phone.PersonalInformation;

namespace Sincro.Core
{
    /// <summary>
    /// Writes Google people into the phone's contact store. The store is opened with system access
    /// ReadOnly, so the People app cannot edit these contacts (one-way Google -> phone).
    /// </summary>
    public static class ContactWriter
    {
        private static readonly string[] ManagedKeys =
        {
            KnownContactProperties.MobileTelephone,
            KnownContactProperties.Telephone,
            KnownContactProperties.WorkTelephone,
            KnownContactProperties.Email,
            KnownContactProperties.WorkEmail,
            KnownContactProperties.OtherEmail,
        };

        public static Task<ContactStore> OpenStoreAsync()
        {
            return ContactStore.CreateOrOpenAsync(ContactStoreSystemAccessMode.ReadOnly, ContactStoreApplicationAccessMode.ReadOnly).AsTask();
        }

        public static async Task UpsertAsync(ContactStore store, ContactModel m)
        {
            string remoteId = RemoteIds.For(m.ResourceName);
            StoredContact sc = await store.FindContactByRemoteIdAsync(remoteId);
            if (sc == null)
            {
                sc = new StoredContact(store);
                sc.RemoteId = remoteId;
                await sc.SaveAsync();
            }
            sc.GivenName = m.GivenName ?? "";
            sc.FamilyName = m.FamilyName ?? "";
            sc.DisplayName = string.IsNullOrEmpty(m.DisplayName) ? "(senza nome)" : m.DisplayName;

            IDictionary<string, object> props = await sc.GetPropertiesAsync();
            foreach (string key in ManagedKeys) props.Remove(key);

            // Phones: first of each kind (ContactMapper already keeps one per kind).
            foreach (KeyValuePair<PhoneKind, string> p in m.Phones)
            {
                switch (p.Key)
                {
                    case PhoneKind.Mobile: props[KnownContactProperties.MobileTelephone] = p.Value; break;
                    case PhoneKind.Work: props[KnownContactProperties.WorkTelephone] = p.Value; break;
                    default: props[KnownContactProperties.Telephone] = p.Value; break;
                }
            }
            foreach (KeyValuePair<EmailKind, string> e in m.Emails)
            {
                switch (e.Key)
                {
                    case EmailKind.Work: props[KnownContactProperties.WorkEmail] = e.Value; break;
                    case EmailKind.Other: props[KnownContactProperties.OtherEmail] = e.Value; break;
                    default: props[KnownContactProperties.Email] = e.Value; break;
                }
            }
            await sc.SaveAsync();
        }

        public static async Task DeleteAsync(ContactStore store, string resourceName)
        {
            StoredContact sc = await store.FindContactByRemoteIdAsync(RemoteIds.For(resourceName));
            if (sc != null) await store.DeleteContactAsync(sc.Id);
        }
    }
}
