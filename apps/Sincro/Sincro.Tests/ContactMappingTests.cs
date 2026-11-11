using System.Collections.Generic;
using System.Linq;
using Newtonsoft.Json.Linq;
using Sincro.Logic;
using Xunit;

namespace Sincro.Tests
{
    // Shapes follow the People API reference (people.connections.list), trimmed.
    public class ContactMappingTests
    {
        private const string Person = @"{
          ""resourceName"": ""people/c1234567890"",
          ""etag"": ""%Eg=="",
          ""names"": [
            { ""metadata"": { ""primary"": false, ""source"": { ""type"": ""CONTACT"", ""id"": ""1"" } },
              ""displayName"": ""Mario R."", ""givenName"": ""Mario"" },
            { ""metadata"": { ""primary"": true, ""source"": { ""type"": ""CONTACT"", ""id"": ""1"" } },
              ""displayName"": ""Mario Rossi"", ""familyName"": ""Rossi"", ""givenName"": ""Mario"" }
          ],
          ""phoneNumbers"": [
            { ""value"": ""+39 333 1234567"", ""canonicalForm"": ""+393331234567"", ""type"": ""mobile"", ""formattedType"": ""Mobile"" },
            { ""value"": ""02 1234567"", ""canonicalForm"": ""+39021234567"", ""type"": ""work"", ""formattedType"": ""Work"" },
            { ""value"": ""06 7654321"", ""type"": ""home"" },
            { ""value"": ""+39 340 0000000"", ""canonicalForm"": ""+393400000000"", ""type"": ""mobile"" }
          ],
          ""emailAddresses"": [
            { ""value"": ""mario@example.com"", ""type"": ""home"" },
            { ""value"": ""mario.rossi@work.example"", ""type"": ""work"" },
            { ""value"": ""extra@example.com"" }
          ],
          ""organizations"": [
            { ""name"": ""Acme Srl"", ""title"": ""Ingegnere"", ""current"": true }
          ]
        }";

        private static ContactModel Map(string json)
        {
            return ContactMapper.Map(GoogleJsonReader.Parse(json));
        }

        [Fact]
        public void Names_use_primary_entry()
        {
            ContactModel c = Map(Person);
            Assert.Equal("people/c1234567890", c.ResourceName);
            Assert.Equal("Mario", c.GivenName);
            Assert.Equal("Rossi", c.FamilyName);
            Assert.Equal("Mario Rossi", c.DisplayName);
        }

        [Fact]
        public void Phone_types_map_to_mobile_work_and_telephone_and_first_of_each_kind_wins()
        {
            ContactModel c = Map(Person);
            var byKind = c.Phones.ToDictionary(p => p.Key, p => p.Value);
            Assert.Equal("+39 333 1234567", byKind[PhoneKind.Mobile]);   // second mobile dropped
            Assert.Equal("02 1234567", byKind[PhoneKind.Work]);
            Assert.Equal("06 7654321", byKind[PhoneKind.Telephone]);     // "home" -> Telephone
            Assert.Equal(3, c.Phones.Count);
        }

        [Theory]
        [InlineData("mobile", PhoneKind.Mobile)]
        [InlineData("MOBILE", PhoneKind.Mobile)]
        [InlineData("work", PhoneKind.Work)]
        [InlineData("home", PhoneKind.Telephone)]
        [InlineData("other", PhoneKind.Telephone)]
        [InlineData("main", PhoneKind.Telephone)]
        [InlineData("homeFax", PhoneKind.Telephone)]
        [InlineData(null, PhoneKind.Telephone)]
        public void PhoneKindFor_maps_google_types(string googleType, PhoneKind expected)
        {
            Assert.Equal(expected, ContactMapper.PhoneKindFor(googleType));
        }

        [Fact]
        public void Emails_work_goes_to_work_slot_home_and_untyped_share_personal_slot()
        {
            ContactModel c = Map(Person);
            var byKind = c.Emails.ToDictionary(e => e.Key, e => e.Value);
            Assert.Equal("mario@example.com", byKind[EmailKind.Personal]);
            Assert.Equal("mario.rossi@work.example", byKind[EmailKind.Work]);
            Assert.Equal(2, c.Emails.Count); // the untyped third address has no free slot
        }

        [Fact]
        public void Organization_company_and_title()
        {
            ContactModel c = Map(Person);
            Assert.Equal("Acme Srl", c.Company);
            Assert.Equal("Ingegnere", c.JobTitle);
        }

        [Fact]
        public void Deleted_entry_from_incremental_sync_has_only_resource_name()
        {
            ContactModel c = Map(@"{ ""resourceName"": ""people/c42"", ""metadata"": { ""deleted"": true } }");
            Assert.True(c.Deleted);
            Assert.Equal("people/c42", c.ResourceName);
            Assert.Empty(c.Phones);
        }

        [Fact]
        public void Name_is_built_from_parts_when_displayName_missing()
        {
            ContactModel c = Map(@"{ ""resourceName"": ""people/c7"", ""names"": [ { ""givenName"": ""Anna"", ""familyName"": ""Bianchi"" } ] }");
            Assert.Equal("Anna Bianchi", c.DisplayName);
        }

        [Fact]
        public void Person_without_resourceName_is_ignored()
        {
            Assert.Null(Map(@"{ ""names"": [] }"));
        }

        [Fact]
        public void Blank_phone_values_are_skipped()
        {
            ContactModel c = Map(@"{ ""resourceName"": ""people/c8"", ""phoneNumbers"": [ { ""value"": """", ""type"": ""mobile"" }, { ""value"": ""+39 3"", ""type"": ""work"" } ] }");
            Assert.Equal(1, c.Phones.Count);
            Assert.Equal(PhoneKind.Work, c.Phones[0].Key);
        }
    }
}
