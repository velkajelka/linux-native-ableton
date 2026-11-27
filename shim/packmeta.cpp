// Pack browser metadata: Windows path separators in "Ableton Folder Info/Metadata.cfg"
// (MENUFIX_PACK_META=0 turns this off).
//
// A factory pack stores its folders and files in Metadata.cfg as UTF-16LE strings, and
// ships them with backslashes: "Ableton Folder Info\Previews\Drums". The Core Library's
// file, and everything Live writes itself, use '/'. Live resolves what a pack folder
// contains from that map rather than from the directory, so on Linux a backslashed key
// never matches the path Live asks about: a pack lists its first-level folders and
// nothing below them (Beat Tools' Drums folder showed empty while holding 30+ kits).
// Verified on Live 12.4.7: after the separators are rewritten, the pack's entries are
// indexed and the browser lists them.
//
// The rewrite is one byte for one byte (U+005C -> U+002F in UTF-16LE), so lengths and
// offsets inside the file stay valid, and the file is replaced atomically next to itself.
// It runs once at start-up, before Live starts Ableton Index, so a pack Live installed
// itself is fixed on the next start. Files without such a byte are left alone, which
// makes it idempotent and cheap (a few hundred KB are read per pack).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void packMetaLog(const char *line) {
  const char *out = getenv("MENUFIX_DEBUG");
  if (!out)
    return;
  if (FILE *f = fopen(out, "a")) {
    fputs(line, f);
    fclose(f);
  }
}

void packMetaLogFixed(const std::string &path, long n) {
  char line[1024];
  snprintf(line, sizeof line, "packmeta: %s: %ld separators\n", path.c_str(), n);
  packMetaLog(line);
}

bool readAll(const std::string &path, std::vector<unsigned char> &out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(st.st_size));
  const size_t n = fread(out.data(), 1, out.size(), f);
  fclose(f);
  if (n != out.size()) {
    out.clear();
    return false;
  }
  return true;
}

// Replaces 5C 00 with 2F 00; returns how many were replaced.
long normalize(std::vector<unsigned char> &data) {
  long n = 0;
  for (size_t i = 0; i + 1 < data.size(); i++) {
    if (data[i] == 0x5c && data[i + 1] == 0x00) {
      data[i] = 0x2f;
      n++;
    }
  }
  return n;
}

bool writeAtomically(const std::string &path, const std::vector<unsigned char> &data, mode_t mode) {
  const std::string tmp = path + ".packmeta";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  const size_t n = fwrite(data.data(), 1, data.size(), f);
  const bool ok = n == data.size();
  fflush(f);
  fclose(f);
  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }
  chmod(tmp.c_str(), mode);
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

// Fixes one pack root; returns true when the file was rewritten.
bool fixPack(const std::string &pack) {
  const std::string meta = pack + "/Ableton Folder Info/Metadata.cfg";
  struct stat st;
  if (stat(meta.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return false;
  std::vector<unsigned char> data;
  if (!readAll(meta, data))
    return false;
  const long n = normalize(data);
  if (!n)
    return false;
  if (!writeAtomically(meta, data, st.st_mode & 07777)) {
    char line[1024];
    snprintf(line, sizeof line, "packmeta: could not rewrite %s\n", meta.c_str());
    packMetaLog(line);
    return false;
  }
  packMetaLogFixed(meta, n);
  return true;
}

} // namespace

void fixPackMetaPaths() {
  if (const char *off = getenv("MENUFIX_PACK_META"); off && *off == '0')
    return;
  const char *home = getenv("HOME");
  if (!home || !*home)
    return;
  const std::string root = std::string(home) + "/Music/Ableton/Factory Packs";
  DIR *dir = opendir(root.c_str());
  if (!dir)
    return;
  int packs = 0;
  while (dirent *ent = readdir(dir)) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    if (fixPack(root + "/" + ent->d_name))
      packs++;
  }
  closedir(dir);
  if (packs) {
    char line[128];
    snprintf(line, sizeof line, "packmeta: rewrote the metadata of %d pack(s)\n", packs);
    packMetaLog(line);
  }
}
