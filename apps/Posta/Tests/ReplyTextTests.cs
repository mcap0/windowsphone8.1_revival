using System;
using Posta.Core.Logic;
using Posta.Core.Model;
using Xunit;

namespace Posta.Tests
{
    public class ReplyTextTests
    {
        [Theory]
        [InlineData("Ciao", "Re: Ciao")]
        [InlineData("Re: Ciao", "Re: Ciao")]
        [InlineData("RE: Ciao", "RE: Ciao")]
        [InlineData("re[2]: Ciao", "re[2]: Ciao")]
        [InlineData("  Ciao  ", "Re: Ciao")]
        [InlineData("", "Re: ")]
        public void ReplySubject_AddsOrKeepsRePrefix(string input, string expected)
        {
            Assert.Equal(expected, ReplyText.ReplySubject(input));
        }

        [Fact]
        public void ReplySubject_NullIsTreatedAsEmpty()
        {
            Assert.Equal("Re: ", ReplyText.ReplySubject(null));
        }

        [Theory]
        [InlineData("Mario Rossi <mario@example.it>", "mario@example.it")]
        [InlineData("mario@example.it", "mario@example.it")]
        [InlineData("  <a@b.c> ", "a@b.c")]
        public void ExtractAddress_ReturnsBareAddress(string from, string expected)
        {
            Assert.Equal(expected, ReplyText.ExtractAddress(from));
        }

        [Fact]
        public void Quote_PrefixesEveryLineAndAddsHeader()
        {
            var quoted = ReplyText.Quote("prima\r\n\r\nseconda", "Mario <m@x.it>", "01/02/2026 10:00");

            Assert.StartsWith("\r\n\r\nIl 01/02/2026 10:00, Mario <m@x.it> ha scritto:\r\n", quoted);
            Assert.Contains("> prima\r\n", quoted);
            Assert.Contains(">\r\n", quoted);
            Assert.Contains("> seconda\r\n", quoted);
        }

        [Fact]
        public void Quote_NormalisesBareCarriageReturns()
        {
            var quoted = ReplyText.Quote("uno\rdue", "a@b.c", "x");
            Assert.Contains("> uno\r\n> due\r\n", quoted);
        }

        [Fact]
        public void ReferencesFor_AppendsMessageIdToExistingReferences()
        {
            var original = new MailContent { References = "<1@x>", MessageId = "<2@x>" };
            Assert.Equal("<1@x> <2@x>", ReplyText.ReferencesFor(original));
        }

        [Fact]
        public void ReferencesFor_WithoutReferencesUsesMessageIdOnly()
        {
            var original = new MailContent { MessageId = "<2@x>" };
            Assert.Equal("<2@x>", ReplyText.ReferencesFor(original));
        }

        [Fact]
        public void PlainFromHtml_StripsTagsScriptsAndDecodesEntities()
        {
            var text = ReplyText.PlainFromHtml("<style>p{}</style><p>Ciao &amp; <b>mondo</b></p><script>alert(1)</script>&lt;ok&gt;");
            Assert.Contains("Ciao & mondo", text);
            Assert.Contains("<ok>", text);
            Assert.DoesNotContain("alert", text);
            Assert.DoesNotContain("<b>", text);
        }
    }
}
