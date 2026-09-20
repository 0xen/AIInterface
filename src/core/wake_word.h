#pragma once
// M12.2. The wake phrase: how a user-typed word is compared against what the
// recogniser heard, decided in one place so that the app, the settings panel
// and the test all ask the same question.
//
// ## Why this is a file and not three lines inside VoiceSession
//
// Everything here is standard-library-only and pure, which is what lets
// `wake_word_test` exercise the whole rule — including every near-miss — with
// no microphone, no engines and no window. The one part of M12.2 that can be
// got wrong silently is the comparison, and it is the one part that can be
// proved on a build machine.
//
// ## The rule
//
// **Normalise both sides, then plain substring.** `normalise_wake()` throws
// away everything that is not a letter or a digit, folds ASCII and full-width
// Latin to lower case, and leaves every other character alone. A match is then
// `heard.find(phrase) != npos`.
//
// That is deliberately *not* a word-boundary match, and the reason is
// Japanese. `language.enabled` is "en,ja" by default; Japanese is written
// without spaces, so a rule built on `\b` or on splitting at whitespace
// matches nothing at all in half the languages this app supports. A rule that
// worked in one language and silently failed in the other would be the same
// class of defect this project has already shipped once, when a byte-length
// truncation cut a multi-byte character in half. So there are no boundaries,
// in either language, and the floor below is what pays for their absence.
//
// Throwing separation away is also what makes a *two-word* phrase usable at
// all: "Hey Aria" comes back from this recogniser as "hey aria", "Hey, Aria!"
// or "heyaria" depending on the chunk boundary, and all three have to be the
// same phrase. The price, stated plainly and checked in the test, is that the
// phrase matches inside a longer word ("aria" in "malaria") and also matches a
// decode that pulled it apart ("a ri a"). Both are the one cost of having no
// boundaries, and `kWakeMinChars` is the only thing standing against them.
//
// ## Which way it errs
//
// **Towards false negatives.** A false positive opens the microphone to
// Claude, which is the user's privacy; a false negative costs them saying the
// word again. Three choices follow from that, and all three make matching
// harder rather than easier:
//
//  1. **A floor of `kWakeMinChars` characters** (counted as codepoints, not
//     bytes). Without word boundaries a two-character phrase is inside an
//     enormous number of ordinary words in both languages. A phrase under the
//     floor is refused outright — the panel says so — rather than armed and
//     firing all day.
//  2. **No fuzziness at all.** No edit distance, no homophone list, no
//     phonetic fold. The recogniser has to have actually decoded the word. A
//     "Nova" heard as "no ver" does not match, and the user repeats it.
//  3. **Nothing about the utterance that woke the app is ever sent.** That is
//     not in this file — see `VoiceSession` — but it is the same decision: the
//     cost of a false positive is capped at an open microphone the user can
//     see, never at a sentence they did not mean to send.
//
// An empty phrase means the feature is off, which is the default and the same
// spelling `timing.listen_timeout`'s `0` already uses: one value, no separate
// flag that can contradict it.
#include <string>

namespace aii {

// The shortest phrase that may be armed, in characters. Three, because two is
// inside "to", "in", "re" and a very large number of kana pairs, and this rule
// has no word boundaries to lean on. See the header comment.
inline constexpr int kWakeMinChars = 3;

// Letters and digits only, lower case, everything else dropped. UTF-8 in,
// UTF-8 out. Multi-byte characters are copied whole — the one operation in
// here that touches bytes individually is the ASCII test, and an ASCII byte
// can never occur inside a multi-byte sequence, so a character cannot be cut.
std::string normalise_wake(const std::string& text);

// How many characters (not bytes) `normalise_wake(text)` leaves.
int wake_chars(const std::string& text);

// Is this phrase one the app will arm? False for empty — which is "off" — and
// false for anything under the floor.
bool wake_phrase_armed(const std::string& phrase);

// Why it will not be armed, in one sentence for the panel, or empty when it
// will be. An empty phrase gets an empty answer: off is not a problem.
std::string wake_phrase_problem(const std::string& phrase);

// Did `heard` contain `phrase`? False whenever the phrase is not armed, so no
// caller has to remember to check that first.
bool wake_match(const std::string& heard, const std::string& phrase);

}  // namespace aii
