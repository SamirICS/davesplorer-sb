// The git repository a folder belongs to, and the web pages of its remotes.
// Read straight from the repository's own files: no git program needed,
// and nothing is run.
#pragma once

#include <string>
#include <vector>

namespace app
{

struct GitRemote
{
    std::string name;      // "origin"
    std::string url;       // as configured, with any user name or token removed
    std::string webUrl;    // the remote's web page, "" when it has none (a local path)
};

// The remotes of the repository holding `path`: the folder itself or the
// nearest folder above it with a .git. Plain repositories, linked worktrees
// and submodules all count. Empty when the folder is in no repository or
// the repository has no remotes. `repoRoot`, when given, receives the
// folder the .git was found in.
std::vector<GitRemote> FindGitRemotes(const std::string& path, std::string* repoRoot = nullptr);

// The https page for a remote URL: scp-style and ssh:// addresses become
// https on the same host, ".git" and credentials are dropped, and Azure
// DevOps' ssh form is mapped to its web form. "" for anything that is not
// a network remote, such as a local path or file:// URL.
std::string RemoteWebUrl(const std::string& remoteUrl);

// The URL without the user name and password that https remotes may carry.
std::string StripUrlCredentials(const std::string& url);

} // namespace app
