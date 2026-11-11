using Posta.Core.Logic;
using Xunit;

namespace Posta.Tests
{
    public class BodySelectorTests
    {
        [Fact]
        public void PrefersPlainWhenBothPresent()
        {
            var choice = BodySelector.Choose("testo", "<b>html</b>");
            Assert.Equal(BodyKind.Plain, choice.Kind);
            Assert.Equal("testo", choice.Text);
        }

        [Fact]
        public void FallsBackToHtmlWhenPlainIsBlank()
        {
            var choice = BodySelector.Choose("   \r\n", "<b>html</b>");
            Assert.Equal(BodyKind.Html, choice.Kind);
            Assert.Equal("<b>html</b>", choice.Text);
        }

        [Fact]
        public void EmptyWhenNothingUsable()
        {
            Assert.Equal(BodyKind.Empty, BodySelector.Choose(null, "  ").Kind);
        }
    }

    public class HtmlPreparerTests
    {
        private const string Remote = "<p>Ciao</p><img src=\"https://tracker.example/p.gif\"><img src='http://x/y.png'><img src=\"cid:logo\">";

        [Fact]
        public void CountsOnlyRemoteImages()
        {
            Assert.Equal(2, HtmlPreparer.CountRemoteImages(Remote));
        }

        [Fact]
        public void RemoteImagesAreBlockedByDefault()
        {
            var prepared = HtmlPreparer.Prepare(Remote, false);
            Assert.DoesNotContain(" src=\"https://tracker", prepared);
            Assert.DoesNotContain(" src='http://x", prepared);
            Assert.Contains("data-blocked-src", prepared);
            Assert.Contains("src=\"cid:logo\"", prepared);
        }

        [Fact]
        public void RemoteImagesKeptWhenAllowed()
        {
            var prepared = HtmlPreparer.Prepare(Remote, true);
            Assert.Contains("src=\"https://tracker.example/p.gif\"", prepared);
            Assert.DoesNotContain("data-blocked-src", prepared);
        }

        [Fact]
        public void ScriptsHandlersAndFramesAreRemoved()
        {
            var prepared = HtmlPreparer.Prepare(
                "<html><head><title>t</title></head><body onload=\"x()\"><script>evil()</script>" +
                "<iframe src=\"http://a\"></iframe><a href=\"javascript:evil()\">k</a>ok</body></html>", true);
            Assert.DoesNotContain("evil()", prepared);
            Assert.DoesNotContain("<iframe", prepared);
            Assert.DoesNotContain("onload", prepared);
            Assert.DoesNotContain("<title>", prepared);
            Assert.Contains("ok", prepared);
        }

        [Fact]
        public void OutputIsUtf8DocumentWithViewport()
        {
            var prepared = HtmlPreparer.Prepare("<p>perché</p>", false);
            Assert.StartsWith("<!DOCTYPE html>", prepared);
            Assert.Contains("<meta charset=\"utf-8\">", prepared);
            Assert.Contains("perché", prepared);
        }
    }
}
