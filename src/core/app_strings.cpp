#include "core/app_strings.h"

#include <atomic>

#include "core/text_util.h"

namespace aii {
namespace {

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------
//
// On the Japanese column: these are **heard**, in 四国めたん's voice, so they
// are written as spoken sentences rather than as translations of the English.
// Where English hedges with "Sorry, I have not set that up" the Japanese says
// the same thing the way a person says it out loud; where English names a
// value in the middle of a sentence, the Japanese puts it where Japanese wants
// it, which is what the `{}` slots are for. No keigo: the app talks to one
// person it already knows, and 丁寧語 on an error line reads as an apology
// from a machine rather than from her.
//
// Order must match `enum class Msg` exactly; the test asserts the table is the
// same length and that every row has English in it.
const AppLine kLines[] = {
    // --- WorkerPool --------------------------------------------------------
    {"{} paused.", "{} はいったん止めたよ。"},
    {"Paused.", "いったん止めたよ。"},
    {"{} failed: {}", "{} は失敗しちゃった: {}"},
    // `{}` is one of the Fail* lines below -- never the CLI's own words, which
    // stay on the panel row and in the log where they are read rather than
    // heard.
    {"The task failed. {}", "うまくいかなかったみたい。{}"},
    // `{}` here is the worker's first sentence -- the model's words, so it is
    // already in the user's language and must not be touched.
    {"{} finished. {}", "{} が終わったよ。{}"},
    {"Finished. {}", "終わったよ。{}"},

    // --- Why a worker failed ----------------------------------------------
    //
    // Every one of these is a whole sentence that follows another whole
    // sentence ("The task failed. ..."), so they are short and they name
    // nothing: no process, no exit code, no flag, no path. A person who has
    // never opened a terminal has to be able to act on them, and what they can
    // act on is "it never got going" versus "it stopped part-way".
    {"There is already one of those running.", "それはもう動いてるよ。"},
    {"It never got going.", "そもそも動き出さなかったよ。"},
    {"It had already stopped, so the work never started.",
     "もう止まってたから、作業は始まってもいないんだ。"},
    {"It stopped before it finished.", "途中で止まっちゃった。"},
    {"It was stopped before it finished.", "途中で止められちゃった。"},
    {"I could not hand the work over to it.", "作業をうまく渡せなかったよ。"},
    {"I could not reach it.", "つながらなかったよ。"},
    {"It has used up what it is allowed for now.", "今は使える分を使い切っちゃったみたい。"},
    {"It would not do that one.", "それはやらないって言われちゃった。"},
    // The unrecognised one. It says the reason exists and where it is, rather
    // than pretending there is none.
    {"I cannot say why, but the reason is on screen.",
     "理由はうまく言えないんだけど、画面に出してるよ。"},

    // --- Workers addressed by voice ---------------------------------------
    {"Could not start worker {}. {}", "{} を始められなかったよ。{}"},
    {"No running worker called {}.", "{} っていう動いてる作業はないよ。"},
    {"No worker called {}.", "{} っていう作業はないよ。"},

    // --- A schedule the app would not accept ------------------------------
    {"Sorry, I have not set that up. I need to know how long, and under a day.",
     "ごめん、それは用意できなかった。どれくらい先か、一日以内で教えて。"},
    {"Sorry, I have not set that up. I need to know which folder to do it in.",
     "ごめん、それは用意できなかった。どのフォルダでやればいいか教えて。"},
    {"Sorry, I have not set that up. I need the full path of the folder.",
     "ごめん、それは用意できなかった。フォルダのフルパスがいるんだ。"},
    {"Sorry, I have not set that up. I am not sure what you wanted me to do then.",
     "ごめん、それは用意できなかった。そのとき何をすればいいのか分からなかったんだ。"},
    {"Sorry, I have not set that up. I am already keeping track of too many things.",
     "ごめん、それは用意できなかった。もう抱えてる予定が多すぎるんだ。"},

    // --- A schedule firing -------------------------------------------------
    {"That is the time you asked me to tell you about.", "言われてた時間だよ。"},
    {"Scheduled worker {} could not start in {}.", "予約してた作業 {} を {} で始められなかったよ。"},
    {"I could not start the thing I put aside for you, and it has not run.",
     "あずかってた用事を始められなかったよ。まだ何もやれてないんだ。"},
    {"Something I set aside for you has finished, but I could not tell you how it went.",
     "あずかってた用事は終わったんだけど、どうなったかは伝えられないんだ。"},

    // --- A cancel that missed ---------------------------------------------
    {"That one had already gone off, so there was nothing to stop.",
     "それはもう時間が来ちゃってたから、止めるものはなかったよ。"},
    {"Those had already gone off, so there was nothing to stop.",
     "どれももう時間が来ちゃってたから、止めるものはなかったよ。"},
    {"I stopped the rest, but one of those had already gone off.",
     "ほかのは止めたけど、ひとつはもう時間が来ちゃってたよ。"},
    {"I stopped the rest, but some of those had already gone off.",
     "ほかのは止めたけど、いくつかはもう時間が来ちゃってたよ。"},

    // --- Shutting down with schedules still pending ------------------------
    {"Closing with {} thing still to do: ", "やり残し {} 件のまま閉じるよ: "},
    {"Closing with {} things still to do: ", "やり残し {} 件のまま閉じるよ: "},

    // --- The AI changing its own settings (M3.14) --------------------------
    //
    // The two questions are the point of the milestone, so they say the cost
    // in the order it is paid: the restart is the mechanism, forgetting is
    // what it costs, and the question comes last so the user answers the
    // thing they just heard. "Restart" on its own is the word the user used
    // when they asked for this and it is not enough on its own any more --
    // since M3.12 the child is replaced at once and the conversation goes
    // with it, so the sentence has to say that out loud.
    {"Switching model means starting Claude again, so I would forget everything we have said. "
     "Shall I?",
     "モデルを変えるにはClaudeを起動し直すことになるから、今まで話したことは全部忘れちゃう。やっていい？"},
    {"Changing what I am allowed to do means starting Claude again, so I would forget everything "
     "we have said. Shall I?",
     "できることを変えるにはClaudeを起動し直すことになるから、今まで話したことは全部忘れちゃう。やっ"
     "ていい？"},
    // Deliberately not "done" on its own: the change is real and stored, and
    // the thing it decides happens once, a second after launch.
    {"That is saved. It takes effect the next time the app starts.",
     "保存したよ。次にアプリを起動したときから効くようになる。"},
    {"That one is the inspector window's own position -- it writes it itself, so anything I put "
     "there would be overwritten.",
     "それはインスペクタのウィンドウが自分で書いてる位置なんだ。僕が入れても上書きされちゃうよ。"},
    {"That one is read once before the window exists, so there is nothing running that can change "
     "it. It can be edited in the file by hand.",
     "それはウィンドウができる前に一度だけ読まれる設定だから、動いてる間には変えられないんだ。ファイル"
     "を直接書き換えればいけるよ。"},
    {"That is the file format's own field rather than a setting.",
     "それは設定じゃなくて、ファイル形式そのものの項目なんだ。"},
    // {key}. The key is said because it is the one piece of information the
    // user cannot get any other way -- it is what *I* reached for, and they
    // are the only one who can tell me it was wrong.
    {"There is no setting called {}, so I have left the file alone.",
     "{} っていう設定はないから、ファイルはそのままにしたよ。"},
    {"{} does not take that value, so I have left it as it was.",
     "{} はその値を取らないから、そのままにしておいたよ。"},
    // M10.5. Not "I cannot", which invites being asked again, but "that one is
    // yours" -- the honest shape of it. The user is told where the switch is,
    // because a refusal that does not say where to go is a dead end.
    {"That one is yours to set, not mine -- it decides whether my own scripts may run. It is "
     "in the settings panel, under Scripts.",
     "それは僕じゃなくて、あなたが決める設定なんだ。僕の書いたスクリプトを動かしていいかどうかのスイッ"
     "チだからね。設定パネルのScriptsのところにあるよ。"},
    // {name}. The one the user's instruction is about. It names the script,
    // says plainly that it is waiting, and ends with the thing to do -- so the
    // reply is a handover rather than an apology.
    {"I have a script called {}, but it is not armed yet, so I cannot run it. Open the settings "
     "panel, find it under Scripts and press Confirm, and then I can.",
     "{} っていうスクリプトはあるんだけど、まだ許可されてないから動かせないんだ。設定パネルのScripts"
     "のところで確認を押してもらえたら、使えるようになるよ。"},
    {"I have a script called {}, but running my own scripts is switched off. You can turn it on "
     "in the settings panel, under Scripts.",
     "{} っていうスクリプトはあるんだけど、僕のスクリプトを動かす設定がオフになってるんだ。設定パネル"
     "のScriptsのところでオンにできるよ。"},
    {"There is no script called {}, so I have not run anything.",
     "{} っていうスクリプトはないから、何も動かしてないよ。"},
    {"{} is past the number of scripts I can keep track of, so I cannot run it. Deleting one you "
     "no longer want, under Scripts, would make room.",
     "{} は僕が覚えていられるスクリプトの数を超えちゃってるから動かせないんだ。Scriptsのところでいら"
     "ないものを消してもらえれば空くよ。"},
    // --- Remembering ---------------------------------------------------------
    // All four say what did not happen and, where there is one, the way out.
    // "Full" names the remedy because the user asked for something to be kept
    // and a flat no leaves them nowhere to go.
    {"My memory is full, so I have not saved that. Asking me to forget something would make room.",
     "覚えておける量がいっぱいで、それは保存できなかったんだ。何かを忘れていいって言ってもらえれば空くよ。"},
    {"There is no memory numbered {}, so I have left the list alone.",
     "{} 番の記憶はないから、リストはそのままにしたよ。"},
    {"There was nothing I could save there, so I have not remembered anything.",
     "保存できるものがなかったから、何も覚えてないよ。"},
    {"I could not write that down, so it is not saved.",
     "書き留められなかったから、保存できてないよ。"},
    // --- The context window filling up -------------------------------------
    // The English is the user's own sentence, kept word for word. The Japanese
    // is not a translation of it but the same move in Japanese: a warning that
    // she is about to go quiet for a moment, in her own register, with no
    // apology in it -- nothing has gone wrong.
    {"I just need a moment to do some housekeeping.",
     "ちょっとだけ、整理する時間をもらうね。"},
    // --- The wake phrase ----------------------------------------------------
    // Both are the shortest natural "I heard you, go on" in their language,
    // and the Japanese is not a translation of the English: "Yes?" as a
    // literal はい？ reads as a question about what was just said, where the
    // whole job of this line is to hand the floor back. なに？ is what someone
    // called by name actually says, in the same register the rest of this
    // table uses.
    {"Yes?", "なに？"},
};

static_assert(sizeof(kLines) / sizeof(kLines[0]) == static_cast<size_t>(Msg::Count),
              "app_strings: the table and the Msg enum have drifted apart");

// Both halves of the resolution at the top of app_strings.h. Separate atomics
// rather than one packed value: they are written by different things (the
// settings surface and the turn loop) and neither has to know about the other.
std::atomic<unsigned> g_enabled{0x3};      // bit 0 English, bit 1 Japanese; both on
std::atomic<bool> g_user_japanese{false};  // nothing heard yet reads as English

}  // namespace

const AppLine& app_line(Msg m) {
  const size_t i = static_cast<size_t>(m);
  // A key past the end is a programming error, not a user-facing one, and the
  // honest answer to it is still a sentence rather than a crash in the middle
  // of telling someone their build broke.
  if (i >= static_cast<size_t>(Msg::Count)) return kLines[static_cast<size_t>(Msg::PausedSpoken)];
  return kLines[i];
}

Msg failure_reason(const std::string& client_error) {
  // Lower-cased once; every needle below is already lower case.
  std::string e;
  e.reserve(client_error.size());
  for (char c : client_error)
    e += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  auto has = [&e](const char* needle) { return e.find(needle) != std::string::npos; };

  // Order matters where two needles can both be present. "HTTP 429" is a
  // limit before it is a network error; "claude exited during startup" is a
  // start that failed before it is a process that exited.
  if (has("already running")) return Msg::FailAlreadyRunning;
  if (has("cancel")) return Msg::FailStoppedByUs;
  if (has("refusal")) return Msg::FailWouldNotDo;
  if (has("usage limit") || has("rate limit") || has("429") || has("overloaded"))
    return Msg::FailAtItsLimit;
  if (has("during startup") || has("createprocess") || has("createpipe"))
    return Msg::FailCouldNotStart;
  if (has("not running")) return Msg::FailNeverStarted;
  if (has("exited") || has("exit code")) return Msg::FailStopped;
  if (has("stdin")) return Msg::FailCouldNotSend;
  if (has("winhttp") || has("http ") || has("timed out") || has("timeout") ||
      has("connect"))
    return Msg::FailCouldNotReach;
  // Unrecognised -- including the CLI's own `is_error` result text, which is
  // free prose and can be anything. It degrades to the one line that admits it
  // cannot explain, instead of reading the prose out loud.
  return Msg::FailUnclear;
}

const char* pick(const AppLine& line, AppLang lang) {
  if (lang == AppLang::Japanese && line.ja && *line.ja) return line.ja;
  // English, or a translation that is not there yet. Either way a sentence
  // comes back: never "", never a key name.
  return (line.en && *line.en) ? line.en : "";
}

std::string app_text_in(AppLang lang, Msg m, const std::string& a, const std::string& b) {
  const std::string form = pick(app_line(m), lang);
  std::string out;
  out.reserve(form.size() + a.size() + b.size());
  int slot = 0;
  for (size_t i = 0; i < form.size(); ++i) {
    if (form[i] == '{' && i + 1 < form.size() && form[i + 1] == '}') {
      // A slot with no argument behind it collapses to nothing rather than
      // surviving as "{}" into something the user hears.
      if (slot == 0) out += a;
      else if (slot == 1) out += b;
      ++slot;
      ++i;
      continue;
    }
    out += form[i];
  }
  return out;
}

std::string app_text(Msg m, const std::string& a, const std::string& b) {
  return app_text_in(app_language(), m, a, b);
}

AppLang app_language_for(LanguageSelection enabled, bool user_spoke_japanese) {
  // One language on: it decides, and nothing else is consulted. This is the
  // case the whole task is about -- a user who has switched English off must
  // never hear an English line.
  if (enabled.japanese && !enabled.english) return AppLang::Japanese;
  if (enabled.english && !enabled.japanese) return AppLang::English;
  // Both on (the default), or the repaired neither-on: follow the user.
  return user_spoke_japanese ? AppLang::Japanese : AppLang::English;
}

void set_enabled_languages(LanguageSelection sel) {
  g_enabled.store((sel.english ? 1u : 0u) | (sel.japanese ? 2u : 0u), std::memory_order_relaxed);
}

void note_user_language(const std::string& user_text) {
  // A turn with no letters, digits or kana in it is not evidence of anything,
  // and counting it as English would flip the app out of Japanese on a stray
  // "..." or an empty transcription.
  if (!has_speakable_content(user_text)) return;
  g_user_japanese.store(has_japanese(user_text), std::memory_order_relaxed);
}

AppLang app_language() {
  const unsigned bits = g_enabled.load(std::memory_order_relaxed);
  LanguageSelection sel{(bits & 1u) != 0, (bits & 2u) != 0};
  return app_language_for(sel, g_user_japanese.load(std::memory_order_relaxed));
}

}  // namespace aii
