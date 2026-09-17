#pragma once
// The app's own sentences, in both languages it speaks.
//
// ---------------------------------------------------------------------------
// Why this exists
// ---------------------------------------------------------------------------
//
// Everything the *model* writes already comes back in the language the
// conversation is being held in -- that is the whole reason a scheduled
// worker's report is a turn rather than a string (M2b.4). But a handful of
// sentences are the app's own: a schedule it refused, a worker that failed, a
// deferred job that could not start. Those were all English, so a Japanese
// speaking user heard English **exactly at the moment something had gone
// wrong**, which is the worst moment available for it.
//
// This is a table, not an i18n framework, and it is meant to stay that way:
// two columns, one enum, no catalogue files, no plural machinery beyond the
// one-and-many pairs that already existed in English, and no dependency. If a
// line needs a value in it, it carries `{}` and `app_text()` fills the slots in
// order -- which is also what lets the Japanese put the value where Japanese
// wants it rather than where English did.
//
// ---------------------------------------------------------------------------
// What is deliberately *not* here
// ---------------------------------------------------------------------------
//
//  * Anything the model reads. The `<context>` blocks, the worker's system
//    prompt and `describe_delay()` are machine traffic: the app states the
//    fact in English and the model says it in the user's language. Translating
//    them would be talking to Claude in two languages for no gain.
//  * Log lines. Nobody hears a log.
//  * The window's own chrome -- the status line, the settings panel, the
//    worker panel's one-word activity states. That is the UI surface, it is
//    never spoken, and localising it is the framework this is not.
//
// ---------------------------------------------------------------------------
// Which language a canned line speaks in
// ---------------------------------------------------------------------------
//
// Nothing new decides this. Two facts the app already keeps:
//
//  1. `LanguageSelection` (core/language.h) -- the settings checkboxes, as the
//     session actually resolved them (`VoiceSession::effective_langs()`, which
//     is the one that also knows whether VOICEVOX ever loaded). With exactly
//     one language on, that language wins outright and nothing else is
//     consulted.
//  2. With **both** on, the deciding fact is `has_japanese()` over the user's
//     last turn -- the same predicate, from the same file, that already
//     decides which voice speaks a run of text. Using it here means a canned
//     line is spoken by the voice the user was already hearing, instead of
//     the app answering a Japanese conversation in Kokoro's English.
//
// The alternative for case 2 was "English unless Japanese is the only language
// on". It was rejected on purpose: the default configuration is both-on, so
// that rule would have left the table switched off for the person it was
// written for.
#include <string>

#include "core/language.h"

namespace aii {

// One language, resolved. Not a `LanguageSelection`: a sentence is spoken in
// one language or the other, never in both and never in neither.
enum class AppLang { English, Japanese };

// Every sentence the app says for itself. Grouped by where it is said, and the
// comment on each is the register it has to keep, because these are heard.
enum class Msg {
  // --- WorkerPool, a live worker reaching the end of its run -------------
  WorkerPausedShown,     // transcript/panel, keeps the worker's name
  PausedSpoken,          // spoken, never names the worker (WorkerPool::ReportFn)
  WorkerFailedShown,     // {name}, {reason} -- the raw reason, kept verbatim
  TaskFailedSpoken,      // {reason} -- one of the Fail* lines below, never the raw one
  WorkerFinishedShown,   // {name}, {what it said}
  FinishedSpoken,        // {what it said} -- the model's words, already localised

  // --- Why a worker failed, in words a person can hear -------------------
  //
  // A client error string ("claude process exited: ", "CreateProcess failed
  // (2): ...") is the CLI's own jargon: it names a process, a Win32 call and
  // an exit code, which is exactly what the pre-prompt asks the *model* never
  // to say -- and no prompt can reach these, because the worker never spoke.
  // The sentence is assembled by the app, so the app has to own the wording.
  //
  // `failure_reason()` picks one of these from the raw string; it is the whole
  // of the mapping, and adding a client error string means adding a rule there
  // and, if nothing here fits, a line here.
  FailAlreadyRunning,    // the pool refused: that name is taken
  FailCouldNotStart,     // it never came up (CreateProcess/CreatePipe/died at startup)
  FailNeverStarted,      // it was already gone when the work was handed over
  FailStopped,           // it ended part-way through the task
  FailStoppedByUs,       // cancelled from this side
  FailCouldNotSend,      // the task never reached it
  FailCouldNotReach,     // network, HTTP, a timeout
  FailAtItsLimit,        // usage or rate limit, overloaded
  FailWouldNotDo,        // a refusal came back
  // The one for everything unrecognised. It is deliberately **not** vague
  // about *whether* something failed -- the wrapper around it has already said
  // that -- only about why, and it points at the place the raw reason really
  // is. A vague line here would hide a real failure; the raw string would be
  // jargon; this says "it failed, I cannot say why, look here".
  FailUnclear,

  // --- Workers addressed by voice ---------------------------------------
  SpawnFailed,           // {name}, {error}
  NoRunningWorker,       // {name}
  NoWorker,              // {name}

  // --- A schedule the app would not accept (M2b.3) ----------------------
  // All five open the same way in both languages, because the user hears the
  // refusal first and the reason second.
  RefuseDelay,
  RefuseNoFolder,
  RefuseRelativeFolder,
  RefuseNothingToDo,
  RefuseTooMany,

  // --- A schedule firing (M2b.4) ----------------------------------------
  TimerNoWords,                 // a timer whose words went missing
  DeferredStartFailedShown,     // {name}, {folder}
  DeferredStartFailedSpoken,    // names nothing: the reason is jargon
  ScheduledReportLost,          // it finished and the report could not be made

  // --- A cancel that missed (M2b.5) -------------------------------------
  CancelMissedOne,
  CancelMissedMany,
  CancelPartialOne,
  CancelPartialMany,

  // --- Shutting down with schedules still pending ------------------------
  ClosingWithOne,        // {count}, then the caller appends the list
  ClosingWithMany,       // {count}

  Count,                 // not a message: the number of them, for the test
};

// One row of the table. `ja` may be empty, which means "not translated yet";
// see `pick()`. Public because the test walks the table and because a caller
// that wants to know what is still English can ask.
struct AppLine {
  const char* en;
  const char* ja;
};

// The raw row, untouched.
const AppLine& app_line(Msg m);

// A client's own error string -> the one Fail* line that says what happened in
// words. Matching is case-insensitive substring, first rule wins, and anything
// unrecognised is `FailUnclear` rather than the raw text: a client error string
// that has not been mapped yet must degrade to a plain sentence, never leak.
//
// It returns the key rather than the sentence so that the caller stays in
// charge of the language and the test can check both columns of one row.
Msg failure_reason(const std::string& client_error);

// **The fallback rule, and the only one.** A missing translation falls back to
// English -- never to an empty string, never to the name of the key. A silent
// gap is the one failure worse than the English this table exists to remove:
// the user would simply not be told that the thing they are waiting for is
// never going to happen.
//
// Public, and taking the row rather than the key, so the rule itself can be
// tested against a deliberately half-filled row without having to leave a real
// message untranslated to prove it works.
const char* pick(const AppLine& line, AppLang lang);

// The sentence, in `lang`, with each `{}` replaced by the next argument in
// order. Arguments left empty are still substituted (as nothing), so a slot
// never survives into something the user hears. Extra `{}` beyond the
// arguments given are erased for the same reason.
std::string app_text_in(AppLang lang, Msg m, const std::string& a = std::string(),
                        const std::string& b = std::string());

// The same, in whatever language the app is currently speaking.
std::string app_text(Msg m, const std::string& a = std::string(),
                     const std::string& b = std::string());

// ---------------------------------------------------------------------------
// The current language
// ---------------------------------------------------------------------------
//
// Process-wide and lock-free, because the readers are wherever a canned line is
// said -- including a `WorkerPool` thread deep in core that has never heard of
// the settings window -- and the writers are the settings surface and the turn
// loop. Two plain atomics: a torn read is impossible and a stale one costs a
// single sentence in the previous language, which is what a race here is worth.

// The settings, as the session resolved them. Called whenever they change.
void set_enabled_languages(LanguageSelection sel);

// The user's own turn, so the table can tell which language they are speaking
// when both are switched on. Anything with Japanese in it counts as Japanese --
// the same rule, from the same function, that routes a mixed sentence to
// VOICEVOX. Text with nothing speakable in it is ignored rather than counted as
// English, so a stray "..." does not flip the app back.
void note_user_language(const std::string& user_text);

// The resolution described at the top of this file, exposed so it can be
// tested without touching the globals.
AppLang app_language_for(LanguageSelection enabled, bool user_spoke_japanese);

// What `app_text()` uses.
AppLang app_language();

}  // namespace aii
