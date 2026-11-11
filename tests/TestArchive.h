// Archives made on the spot for the tests, with libarchive's writer, so no
// binary fixtures live in the repository. Linux only, like extracting.
#pragma once

#ifdef __linux__

#include <archive.h>
#include <archive_entry.h>

#include <string>
#include <vector>

namespace testarchive
{

struct Item
{
    std::string path;
    std::string content;   // a file's bytes, or a link's target
    char        type = 'f';   // 'f' file, 'd' folder, 'l' symbolic link
};

// format: a libarchive format name ("zip", "7zip", "gnutar", "raw");
// filter: "gzip", "xz"... or nullptr. A zip with a passphrase is encrypted
// the traditional way, which libarchive reads back.
inline bool Write(const std::string& file, const char* format, const char* filter, const std::vector<Item>& items,
                  const char* passphrase = nullptr)
{
    archive* a = archive_write_new();
    bool ok = archive_write_set_format_by_name(a, format) == ARCHIVE_OK;
    if (filter) ok = ok && archive_write_add_filter_by_name(a, filter) == ARCHIVE_OK;
    if (passphrase)
        ok = ok && archive_write_set_options(a, "zip:encryption=zipcrypt") == ARCHIVE_OK &&
             archive_write_set_passphrase(a, passphrase) == ARCHIVE_OK;
    ok = ok && archive_write_open_filename(a, file.c_str()) == ARCHIVE_OK;
    for (const Item& item : items)
        {
        if (!ok) break;
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, item.path.c_str());
        archive_entry_set_mtime(e, 1700000000, 0);
        if (item.type == 'd')
            {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, 0755);
            }
        else if (item.type == 'l')
            {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_perm(e, 0777);
            archive_entry_set_symlink(e, item.content.c_str());
            }
        else
            {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            archive_entry_set_size(e, (la_int64_t)item.content.size());
            }
        ok = archive_write_header(a, e) == ARCHIVE_OK;
        if (ok && item.type == 'f' && !item.content.empty())
            ok = archive_write_data(a, item.content.data(), item.content.size()) == (la_ssize_t)item.content.size();
        archive_entry_free(e);
        }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok;
}

} // namespace testarchive

#endif
