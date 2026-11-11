// Unpacking archives: beside the archive, into a folder named after it, or
// into a folder of the user's choosing.
//
// Everything is unpacked into a hidden folder inside the destination first
// and only moved into place once the whole archive came out, so a failure,
// a wrong passphrase or Cancel leaves the destination exactly as it was.
// What lands is recorded for Edit > Undo.
#pragma once

#include "platform/Archive.h"

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace app
{

struct AppState;

// Whether the name looks like an archive the menu should offer to extract.
// By extension only; office documents and Java/Android packages are zips
// too, but nobody means to unpack a .docx by right-clicking it.
bool IsArchiveName(const std::string& name);

// The name without its archive extension, for the folder it unpacks into:
// "photos.tar.gz" -> "photos", "notes.zip" -> "notes".
std::string ArchiveStem(const std::string& name);

struct ExtractRequest
{
    ExtractRequest() = default;
    ExtractRequest(std::string archive_, std::string destDir_, bool ownFolder_ = false)
        : archive(std::move(archive_)), destDir(std::move(destDir_)), ownFolder(ownFolder_) {}

    std::string archive;
    std::string destDir;
    // Into a new folder in destDir named after the archive, or, when the
    // archive holds one folder and nothing else, that folder itself.
    bool        ownFolder = false;
    // Archives found among what was unpacked are unpacked too, each into
    // its own folder beside it, and so on down.
    bool        nested = false;
    // Once unpacked, the archive (and each one found inside) moves into a
    // folder of this name beside it. Empty leaves them where they are.
    std::string originalsFolder;
};

// The request the settings ask for: whether to go into archives found
// inside, and where originals go afterwards.
struct Settings;
ExtractRequest MakeExtractRequest(const Settings& settings, const std::string& archive, const std::string& destDir,
                                  bool ownFolder);

// How deep archives inside archives are followed, and how many at most
// one extraction unpacks: an archive can hold itself.
inline constexpr int    kMaxNestedDepth = 8;
inline constexpr size_t kMaxNestedArchives = 500;

// What to do when items with the archive's top-level names already exist.
enum class ConflictChoice
{
    Replace,    // the existing ones go to the Recycle Bin first
    Skip,       // the existing ones stay; those items are not unpacked
    KeepBoth,   // the unpacked ones are named "name (2)"
    Cancel,
};

// How the unpacking asks the user things. Each may block until answered.
struct ExtractQuestions
{
    // wrong: a passphrase was tried and did not work. False gives up.
    std::function<bool(bool wrong, std::string& passphrase)> passphrase;
    std::function<ConflictChoice(const std::vector<std::string>& names)> conflict;
    platform::ArchiveProgressFn progress;
    // An archive found inside is starting; its passphrase question and
    // progress are about it from here on.
    std::function<void(const std::string& archive)> started;
};

struct ExtractOutcome
{
    bool ok = false;
    bool cancelled = false;
    std::vector<std::string> placed;     // what now sits in the destination
    std::vector<std::string> replaced;   // what was sent to the Recycle Bin to make room
    std::string movedFrom, movedTo;      // the archive itself, put away in the originals folder
    size_t nestedExtracted = 0;          // archives found inside and unpacked too
    std::string error;
    std::string note;                    // done, but something was left out
};

// The whole of one extraction, on whatever thread calls it. Touches only
// the disk: the caller hands the outcome to FinishExtract.
ExtractOutcome ExtractNow(const ExtractRequest& request, const ExtractQuestions& questions,
                          const std::atomic<bool>& cancel);

// Interface thread: records the outcome for Undo, re-reads the views and
// sets the status message.
void FinishExtract(AppState& state, const ExtractRequest& request, const ExtractOutcome& outcome);

// Extractions in the background, one at a time, in the order asked for.
// The interface polls once a frame and answers the questions an
// extraction stops on.
namespace extractjob
{

enum class Stage
{
    Idle,
    Working,
    NeedPassphrase,
    NeedConflictChoice,
};

struct Status
{
    Stage       stage = Stage::Idle;
    std::string archive;
    std::string destDir;
    size_t      index = 0;          // 1-based position in this run of extractions
    size_t      count = 0;
    float       progress = 0.0f;
    bool        wrongPassphrase = false;
    std::vector<std::string> conflicts;
};

void   Start(const std::vector<ExtractRequest>& requests);
Status Poll();
void   AnswerPassphrase(bool ok, const std::string& passphrase);
void   AnswerConflict(ConflictChoice choice);
// Stops the one running and drops the rest.
void   Cancel();
// Hands finished extractions to FinishExtract; their errors and notes are
// appended to `messages` for the interface to show.
void   Tick(AppState& state, std::vector<std::string>& messages);
void   Shutdown();

} // namespace extractjob

} // namespace app
