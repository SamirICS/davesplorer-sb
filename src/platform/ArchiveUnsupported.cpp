// Archives are read on Linux only for now: everywhere else each call says
// so, and the menu entries that lead here stay greyed out.
#include "platform/Archive.h"

namespace platform
{

namespace
{

bool Unsupported(ArchiveFailure& why, std::string& error)
{
    why = ArchiveFailure::Other;
    error = "Extracting archives is not supported on this system yet.";
    return false;
}

} // namespace

std::string SevenZipProgram()
{
    return {};
}

bool ListArchive(const std::string&, const std::string&, ArchiveListing&, ArchiveFailure& why, std::string& error)
{
    return Unsupported(why, error);
}

bool ExtractArchive(const std::string&, const std::string&, const std::string&, const ArchiveProgressFn&,
                    const std::atomic<bool>&, ArchiveFailure& why, std::string& error, std::string&)
{
    return Unsupported(why, error);
}

} // namespace platform
