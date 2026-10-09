// What goes on the wire, and the text handling under it.
#include "support.hpp"
#include "url.hpp"
#include "utf.hpp"
#include "wire.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

TEST(EnteredKey, TrimsWhatJavaScriptTrimsAndUpperCasesAsItDoes) {
  EXPECT_EQ(enteredKey("  sluice-aaaa\n"), "SLUICE-AAAA");
  // NBSP, U+3000, a BOM, U+2028 and U+205F are white space to String.prototype.trim.
  EXPECT_EQ(enteredKey("\xC2\xA0\xE3\x80\x80\xEF\xBB\xBFkey\xE2\x80\xA8\xE2\x81\x9F"), "KEY");
  // Not white space: U+200B (zero width space) stays.
  EXPECT_EQ(enteredKey("\xE2\x80\x8Bkey"), "\xE2\x80\x8BKEY");
  // Full case mapping: ß is SS, é is É, ﬁ is FI.
  EXPECT_EQ(enteredKey("stra\xC3\x9F" "e"), "STRASSE");
  EXPECT_EQ(enteredKey("caf\xC3\xA9"), "CAF\xC3\x89");
  EXPECT_EQ(enteredKey("\xEF\xAC\x81x"), "FIX");
  EXPECT_EQ(enteredKey(""), std::nullopt);
  EXPECT_EQ(enteredKey(" \t\r\n"), std::nullopt);
}

TEST(EnteredKey, MeasuresTheNormalisedKeyInUtf16Units) {
  EXPECT_TRUE(enteredKey(std::string(120, 'K')));
  EXPECT_FALSE(enteredKey(std::string(121, 'K')));
  EXPECT_TRUE(enteredKey(std::string(120, 'K') + "\n"));
  // An emoji is two units: 119 + 2 is over.
  EXPECT_FALSE(enteredKey(std::string(119, 'K') + "\xF0\x9F\x98\x80"));
  // ß becomes SS after the trim, before the measure: 119 + 2.
  EXPECT_FALSE(enteredKey(std::string(119, 'K') + "\xC3\x9F"));
}

TEST(TrimToLength, CutsWithoutSplittingACharacter) {
  const std::string emoji = "\xF0\x9F\x98\x80";
  EXPECT_EQ(trimToLength(std::string(119, 'D') + emoji, 120), std::string(119, 'D'));
  EXPECT_EQ(trimToLength(std::string(118, 'D') + emoji, 120), std::string(118, 'D') + emoji);
  EXPECT_EQ(trimToLength(std::string(500, 'D'), 120), std::string(120, 'D'));
  EXPECT_EQ(trimToLength(std::string(120, 'D'), 120), std::string(120, 'D'));
  EXPECT_EQ(trimToLength(std::nullopt, 120), std::nullopt);
  // Bytes that are not UTF-8 are a replacement character, as a JavaScript string would hold them.
  EXPECT_EQ(trimToLength(std::string("a\xff"), 120), "a\xEF\xBF\xBD");
}

TEST(ServerErrors, AreReadOnlyFromAStringErrorInAnObject) {
  EXPECT_EQ(errorWord(json{{"error", "limit"}}), "limit");
  EXPECT_EQ(errorWord(json{{"error", 42}}), std::nullopt);
  EXPECT_EQ(errorWord(json::array({"limit"})), std::nullopt);
  EXPECT_EQ(errorWord(json("limit")), std::nullopt);
  EXPECT_EQ(errorWord(json::parse("not json", nullptr, false)), std::nullopt);
  EXPECT_EQ(mapServerError(std::nullopt), keygrant::ActivateError::Network);
  EXPECT_EQ(refusalOf(std::string("limit")), std::nullopt);
}

TEST(Bodies, AreReadAsTheReferenceReadsThem) {
  EXPECT_EQ(parseLeaseBody(json{{"lease", 1}, {"activationId", "a"}}).lease, std::nullopt);
  // A trial's times are any JSON number, floored to a whole millisecond.
  const auto trial = parseTrialBody(json{{"trialStart", 1.9}, {"trialEndsAt", 2}});
  ASSERT_TRUE(trial);
  EXPECT_EQ(trial->trialStart, 1);
  EXPECT_FALSE(parseTrialBody(json{{"trialStart", nullptr}, {"trialEndsAt", 2}}));
  // A body is read as fetch reads it: a BOM dropped, ill-formed UTF-8 replaced.
  EXPECT_EQ(parseJsonText("\xEF\xBB\xBF{\"error\":\"limit\"}")["error"], "limit");
  EXPECT_EQ(parseJsonText("{\"error\":\"li\xffmit\"}")["error"], "li\xEF\xBF\xBDmit");
  EXPECT_TRUE(parseJsonText("<html>").is_discarded());
}

TEST(ReferencedLink, EditsTheQueryAsTextTheReferenceLast) {
  EXPECT_EQ(referencedLink("https://buy.stripe.com/test_abc", "act_device1"),
            "https://buy.stripe.com/test_abc?client_reference_id=act_device1");
  // Its own parameters kept as written and in their order, every client_reference_id left out, the reference last.
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x?prefilled_email=jo%40example.com&client_reference_id=old&a=1"
                           "&client_reference_id=again",
                           "act_1"),
            "https://buy.stripe.com/x?prefilled_email=jo%40example.com&a=1&client_reference_id=act_1");
  // Not re-serialized: a `+` and a part with no `=` stay as written; the reference is encoded as encodeURIComponent does.
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x?q=a+b&flag", "a b/c"),
            "https://buy.stripe.com/x?q=a+b&flag&client_reference_id=a%20b%2Fc");
  // The scheme and host lower-cased, an empty path written as `/`; the fragment kept, an empty one too.
  EXPECT_EQ(referencedLink("HTTPS://Buy.Stripe.com", "act_1"), "https://buy.stripe.com/?client_reference_id=act_1");
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x#pay", "act_1"),
            "https://buy.stripe.com/x?client_reference_id=act_1#pay");
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x?a=1#", "r"), "https://buy.stripe.com/x?a=1&client_reference_id=r#");
  EXPECT_EQ(referencedLink(" https://buy.stripe.com/x?\n", "act_1"), "https://buy.stripe.com/x?client_reference_id=act_1");
}

TEST(ReferencedLink, EncodesWhatTheUrlParserEncodesAndNothingElse) {
  // Each answer is what a WHATWG URL parser gives (Node 22's URL, its search set).
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x?q=a b&n=\"<'>\"&e=%41#a b`", "act_1"),
            "https://buy.stripe.com/x?q=a%20b&n=%22%3C%27%3E%22&e=%41&client_reference_id=act_1#a%20b%60");
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x?city=K\xC3\xB6ln", "act_1"),
            "https://buy.stripe.com/x?city=K%C3%B6ln&client_reference_id=act_1");
  EXPECT_EQ(referencedLink("https://buy.stripe.com/x?a\x01" "b", "r"),
            "https://buy.stripe.com/x?a%01b&client_reference_id=r");
  // encodeURIComponent leaves -_.!~*'() alone, and an http(s) URL's query then encodes its `'`.
  EXPECT_EQ(referencedLink("https://x.test/", "a-_.!~*'()\xC3\xA9"),
            "https://x.test/?client_reference_id=a-_.!~*%27()%C3%A9");
  // A URL that is not http(s) keeps a `'` as it is.
  EXPECT_EQ(referencedLink("custom:pay?it's", "r"), "custom:pay?it's&client_reference_id=r");
}

TEST(ReferencedLink, RefusesALinkThatIsNotAnAbsoluteUrl) {
  for (const char* link : {"", "buy.stripe.com/x", "/relative", "https://", "https:///path", "1http://x", "https:no-slashes"}) {
    EXPECT_THROW(referencedLink(link, "act_1"), std::invalid_argument) << link;
    EXPECT_THROW(absoluteLink(link), std::invalid_argument) << link;
  }
}

TEST(ClaimBodies, AreNothingWhenTheTextIsNotJson) {
  // The parseClaimBody vectors hold every JSON body; text that is not JSON (a portal's page) is the one they cannot.
  const ClaimBody page = parseClaimBody(parseJsonText("<html>"));
  EXPECT_FALSE(page.key || page.activationId || page.lease || page.pending || page.error);
}

TEST(Utf8, DecodesAsTheWhatwgDecoderDoes) {
  // Every ill-formed sequence is one U+FFFD, the bytes after it read again.
  EXPECT_EQ(sanitizeUtf8("a\xffz"), "a\xEF\xBF\xBDz");
  EXPECT_EQ(sanitizeUtf8("\xE2\x82"), "\xEF\xBF\xBD");
  EXPECT_EQ(sanitizeUtf8("\xE2\x82z"), "\xEF\xBF\xBDz");
  EXPECT_EQ(sanitizeUtf8("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // an encoded surrogate
  EXPECT_EQ(sanitizeUtf8("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");  // an overlong form
  EXPECT_EQ(sanitizeUtf8("\xEF\xBF\xBD"), "\xEF\xBF\xBD");  // a real U+FFFD is kept as it is
  EXPECT_EQ(utf16Length("a\xC3\xA9\xF0\x9F\x98\x80"), 4u);
  EXPECT_EQ(utf16ToUtf8(u"a\xD83D"), "a\xEF\xBF\xBD");  // a lone surrogate
  EXPECT_EQ(utf8ToUtf16("\xF0\x9F\x98\x80"), std::u16string(u"\xD83D\xDE00"));
}

}  // namespace
}  // namespace kgtest
