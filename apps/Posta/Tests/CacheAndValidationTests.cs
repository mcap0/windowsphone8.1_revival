using System;
using Posta.Core.Cache;
using Posta.Core.Logic;
using Posta.Core.Model;
using Xunit;

namespace Posta.Tests
{
    public class InboxCacheTests
    {
        [Fact]
        public void RoundTripKeepsMessagesAndDates()
        {
            var when = new DateTimeOffset(2026, 10, 18, 9, 30, 0, TimeSpan.FromHours(2));
            var doc = new InboxCacheDocument
            {
                Account = "a@gmail.com",
                Total = 120,
                Messages =
                {
                    new MailSummary { Uid = 42, From = "Mario <m@x.it>", Subject = "Ciao è è", Date = when, IsSeen = false, MessageId = "<id@x>" },
                },
            };

            var back = InboxCache.Deserialize(InboxCache.Serialize(doc));

            Assert.NotNull(back);
            Assert.Equal("a@gmail.com", back.Account);
            Assert.Equal(120, back.Total);
            Assert.Single(back.Messages);
            var m = back.Messages[0];
            Assert.Equal(42u, m.Uid);
            Assert.Equal("Ciao è è", m.Subject);
            Assert.Equal(when, m.Date);
            Assert.False(m.IsSeen);
            Assert.Equal("<id@x>", m.MessageId);
        }

        [Theory]
        [InlineData(null)]
        [InlineData("")]
        [InlineData("   ")]
        [InlineData("{not json")]
        [InlineData("[1,2,3]")]
        public void BadOrEmptyTextGivesNull(string json)
        {
            Assert.Null(InboxCache.Deserialize(json));
        }
    }

    public class AccountTests
    {
        [Fact]
        public void EffectiveUsernameFallsBackToEmail()
        {
            var s = new AccountSettings { Email = " me@gmail.com " };
            Assert.Equal("me@gmail.com", s.EffectiveUsername);
            s.Username = "other";
            Assert.Equal("other", s.EffectiveUsername);
        }

        [Fact]
        public void GmailDefaultsUseImapAndSmtpSslPorts()
        {
            var s = AccountSettings.GmailDefaults("me@gmail.com");
            Assert.Equal("imap.gmail.com", s.ImapHost);
            Assert.Equal(993, s.ImapPort);
            Assert.Equal("smtp.gmail.com", s.SmtpHost);
            Assert.Equal(465, s.SmtpPort);
        }

        [Fact]
        public void ValidGmailAccountHasNoErrors()
        {
            var errors = AccountValidator.Validate(AccountSettings.GmailDefaults("me@gmail.com"), "abcd efgh ijkl mnop", true);
            Assert.Empty(errors);
        }

        [Fact]
        public void BadEmailAndMissingPasswordAreReported()
        {
            var errors = AccountValidator.Validate(AccountSettings.GmailDefaults("not-an-address"), "", true);
            Assert.Equal(2, errors.Length);
        }

        [Fact]
        public void PasswordNotRequiredWhenAlreadyStored()
        {
            var errors = AccountValidator.Validate(AccountSettings.GmailDefaults("me@gmail.com"), null, false);
            Assert.Empty(errors);
        }

        [Theory]
        [InlineData(0)]
        [InlineData(70000)]
        [InlineData(-1)]
        public void PortOutOfRangeIsReported(int port)
        {
            var s = AccountSettings.GmailDefaults("me@gmail.com");
            s.ImapPort = port;
            var errors = AccountValidator.Validate(s, "pwd", true);
            Assert.Single(errors);
        }
    }
}
