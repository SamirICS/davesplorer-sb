#include "app/GitRemote.h"

#include "platform/Strings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace app
{

namespace
{

namespace fs = std::filesystem;

fs::path U8Path(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

std::string FromPath(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string FirstLine(const fs::path& file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return Trim(line);
}

// A path a git file points at: absolute as written, or relative to the
// folder holding the file.
fs::path Resolve(const fs::path& base, const std::string& target)
{
    const fs::path t = U8Path(target);
    return (t.is_absolute() ? t : base / t).lexically_normal();
}

// The config file that holds a repository's remotes, given the folder its
// .git sits in. A worktree's or submodule's .git is a file naming the real
// git directory, and a linked worktree's git directory names, in
// "commondir", the main one where the config lives.
bool FindConfig(const fs::path& dir, fs::path& config)
{
    std::error_code ec;
    const fs::path dotGit = dir / ".git";
    fs::path gitDir;
    if (fs::is_directory(dotGit, ec))
        {
        gitDir = dotGit;
        }
    else if (fs::is_regular_file(dotGit, ec))
        {
        const std::string line = FirstLine(dotGit);
        if (!StartsWith(line, "gitdir:")) return false;
        gitDir = Resolve(dir, Trim(line.substr(7)));
        }
    else
        {
        return false;
        }
    const fs::path commonDir = gitDir / "commondir";
    if (fs::is_regular_file(commonDir, ec)) gitDir = Resolve(gitDir, FirstLine(commonDir));
    config = gitDir / "config";
    return true;
}

// A config value: inline comments dropped, quotes removed.
std::string ConfigValue(const std::string& raw)
{
    std::string out;
    bool quoted = false;
    for (size_t i = 0; i < raw.size(); ++i)
        {
        const char c = raw[i];
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && (c == '#' || c == ';')) break;
        if (c == '\\' && i + 1 < raw.size()) { out += raw[++i]; continue; }
        out += c;
        }
    return Trim(out);
}

// [remote "origin"] url = ..., in the order the remotes first appear. The
// old [remote.origin] spelling is read too.
std::vector<GitRemote> ReadRemotes(const fs::path& config)
{
    std::vector<GitRemote> out;
    std::ifstream in(config);
    std::string line, remote;
    bool inRemote = false;
    while (std::getline(in, line))
        {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[')
            {
            const size_t close = line.find(']');
            const std::string header = Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            inRemote = false;
            const size_t quote = header.find('"');
            if (quote != std::string::npos)
                {
                const size_t end = header.rfind('"');
                inRemote = platform::ToLowerAscii(Trim(header.substr(0, quote))) == "remote" && end > quote;
                if (inRemote) remote = header.substr(quote + 1, end - quote - 1);
                }
            else if (platform::ToLowerAscii(header).rfind("remote.", 0) == 0)
                {
                inRemote = true;
                remote = header.substr(7);
                }
            continue;
            }
        if (!inRemote || remote.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (platform::ToLowerAscii(Trim(line.substr(0, eq))) != "url") continue;
        const std::string url = ConfigValue(line.substr(eq + 1));
        bool known = false;
        for (const GitRemote& r : out) known = known || r.name == remote;
        if (known || url.empty()) continue;   // a second url line is a push mirror, not the page
        out.push_back(GitRemote{ remote, StripUrlCredentials(url), RemoteWebUrl(url) });
        }
    return out;
}

} // namespace

std::string StripUrlCredentials(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    const size_t start = scheme + 3;
    const size_t end = std::min(url.find('/', start), url.size());
    const size_t at = url.rfind('@', end == url.size() ? std::string::npos : end);
    if (at == std::string::npos || at < start) return url;
    return url.substr(0, start) + url.substr(at + 1);
}

std::string RemoteWebUrl(const std::string& remoteUrl)
{
    const std::string url = Trim(remoteUrl);
    if (url.empty() || url[0] == '/' || url[0] == '.' || url[0] == '~' || url[0] == '\\') return {};
    std::string webScheme = "https";
    std::string host, path;
    const size_t schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos)
        {
        const std::string scheme = platform::ToLowerAscii(url.substr(0, schemeEnd));
        if (scheme == "http") webScheme = "http";
        else if (scheme != "https" && scheme != "ssh" && scheme != "git" && scheme != "git+ssh" && scheme != "ssh+git") return {};
        const std::string rest = StripUrlCredentials(url).substr(schemeEnd + 3);
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return {};
        host = rest.substr(0, slash);
        path = rest.substr(slash);
        // An ssh or git port is not the web server's.
        if (webScheme == "https" && scheme != "https")
            if (const size_t colon = host.rfind(':'); colon != std::string::npos && host.find(']') == std::string::npos) host.resize(colon);
        }
    else
        {
        // scp-like: [user@]host:path. A slash before the colon, or a lone
        // drive letter, makes it a local path instead.
        const size_t colon = url.find(':');
        if (colon == std::string::npos || colon == 0) return {};
        if (url.find('/') < colon || url.find('\\') < colon) return {};
        if (colon == 1 && std::isalpha((unsigned char)url[0])) return {};
        host = url.substr(0, colon);
        if (const size_t at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
        path = url.substr(colon + 1);
        if (path.empty() || path[0] != '/') path = "/" + path;
        }
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (EndsWith(path, ".git")) path.resize(path.size() - 4);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (host.empty() || path.size() <= 1) return {};

    // Azure DevOps answers ssh on its own host with a v3/org/project/repo
    // path; the page for it lives elsewhere and is shaped differently.
    if (platform::ToLowerAscii(host) == "ssh.dev.azure.com" && StartsWith(path, "/v3/"))
        {
        const std::string parts = path.substr(4);
        const size_t a = parts.find('/');
        const size_t b = a == std::string::npos ? a : parts.find('/', a + 1);
        if (b == std::string::npos) return {};
        return "https://dev.azure.com/" + parts.substr(0, a) + "/" + parts.substr(a + 1, b - a - 1) + "/_git/" + parts.substr(b + 1);
        }
    return webScheme + "://" + host + path;
}

std::vector<GitRemote> FindGitRemotes(const std::string& path, std::string* repoRoot)
{
    if (path.empty()) return {};   // This PC
    std::error_code ec;
    fs::path dir = fs::absolute(U8Path(path), ec).lexically_normal();
    if (ec) return {};
    for (;;)
        {
        fs::path config;
        if (FindConfig(dir, config))
            {
            if (repoRoot) *repoRoot = FromPath(dir);
            return ReadRemotes(config);
            }
        const fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) return {};
        dir = parent;
        }
}

} // namespace app
