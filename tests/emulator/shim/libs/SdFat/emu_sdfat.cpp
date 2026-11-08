/* emu_sdfat.cpp - SdFat shim backed by a host directory (EMU_SD_DIR). */
#include <string>
#include <vector>
#include <algorithm>
#include "SdFat.h"
#undef atoi
#undef atol
#undef strtol
#undef strtoul
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "emu_internal.h"
#include <time.h>
static unsigned long long wall_ms() { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000; }

struct EmuFileState {
  int refs;
  int fd;
  bool dir;
  bool open;
  bool werr;
  bool writable;
  std::string path;     /* host path */
  std::string name;     /* name on card */
  std::vector<std::string> entries;
  size_t idx;
  uint32_t pos;
};

static int g_sdlog = -2;
static void sdlog(const char *op, const char *path, long v) {
  if (g_sdlog == -2) g_sdlog = emu_open_log("EMU_SD_LOG");
  if (g_sdlog < 0) return;
  char buf[640];
  int n = snprintf(buf, sizeof buf, "{\"wt\":%llu,\"t\":%u,\"boot\":%d,\"op\":\"%s\",\"path\":\"%s\",\"v\":%ld}\n",
                   wall_ms(), emu_millis32(), emu_boot_count(), op, path, v);
  emu_write_line(g_sdlog, buf, n);
}

static uint64_t g_target;
static void sd_pace(size_t bytes) {
  static long ns = -1;
  if (ns < 0) ns = emu_rt_long("EMU_SD_BYTE_NS", 1000);
  if (!ns || !bytes) return;
  uint64_t now = emu_mono_ns();
  if (g_target < now) g_target = now;
  g_target += (uint64_t)ns * bytes;
  if (g_target > now + 200000ull) emu_sleep_until_ns(g_target);
}

static const char *sd_root() { return emu_env("EMU_SD_DIR"); }
static bool g_card_ok;
/* sd-p2 test variant: the card is "pulled" while EMU_SD_DIR is missing.
 * Like SdFat 2.x, a card error stays until the next begin(). */
static uint8_t g_sderr;
/* p2sd-fix variant: a card directory replaced since begin() (new inode,
 * i.e. reinserted) is not initialized: every op fails until begin(). */
static ino_t g_ino; static bool g_ino_set;
static bool card_gone(void) {
  struct stat st; const char *r = sd_root();
  if (r && stat(r, &st) == 0 && S_ISDIR(st.st_mode)) {
    if (g_ino_set && st.st_ino != g_ino) { g_sderr = 0x20; return true; }
    return false;
  }
  g_sderr = 0x20; return true;
}

static std::string host_path(const char *p) {
  std::string r = sd_root() ? sd_root() : "/nonexistent";
  if (!p || !*p) return r;
  if (p[0] != '/') r += "/";
  r += p;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  return r;
}

static void unref(EmuFileState *s) {
  if (!s) return;
  if (--s->refs == 0) {
    if (s->fd >= 0) close(s->fd);
    delete s;
  }
}

File::File() : _st(NULL) {}
File::File(const File &o) : Stream(), _st(o._st) { if (_st) _st->refs++; }
File &File::operator=(const File &o) {
  if (o._st) o._st->refs++;
  unref(_st);
  _st = o._st;
  return *this;
}
File::~File() { unref(_st); }
bool File::isOpen() const { return _st && _st->open; }
bool File::isDir() const { return isOpen() && _st->dir; }
uint32_t File::size() const {
  if (!isOpen() || _st->dir) return 0;
  struct stat st;
  if (fstat(_st->fd, &st) < 0) return 0;
  return (uint32_t)st.st_size;
}
uint32_t File::position() const { return isOpen() ? _st->pos : 0; }
int File::available() {
  if (!isOpen() || _st->dir) return 0;
  uint32_t n = size() - _st->pos;
  return n > 32767 ? 32767 : (int)n;   /* INT_MAX on AVR */
}
int File::read(void *buf, size_t n) {
  if (!isOpen() || _st->dir) return -1;
  if (card_gone()) return -1;
  ssize_t r = pread(_st->fd, buf, n, _st->pos);
  if (r < 0) return -1;
  _st->pos += (uint32_t)r;
  sd_pace((size_t)r);
  return (int)r;
}
int File::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}
int File::peek() {
  if (!isOpen() || _st->dir) return -1;
  uint8_t b;
  return pread(_st->fd, &b, 1, _st->pos) == 1 ? b : -1;
}
size_t File::write(const uint8_t *buf, size_t n) {
  if (!isOpen() || _st->dir || !_st->writable) { if (_st) _st->werr = true; setWriteError(); return 0; }
  if (emu_env_bool("EMU_SD_READONLY", 0)) { _st->werr = true; setWriteError(); return 0; }
  ssize_t r = pwrite(_st->fd, buf, n, _st->pos);
  if (r < 0) { _st->werr = true; setWriteError(); return 0; }
  _st->pos += (uint32_t)r;
  sd_pace((size_t)r);
  return (size_t)r;
}
size_t File::write(uint8_t b) { return write(&b, 1); }
void File::flush() { sync(); }
bool File::sync() { return isOpen() && !_st->werr; }
bool File::close() {
  if (!isOpen()) return false;
  bool ok = !_st->werr;
  if (_st->writable) sdlog("close_w", _st->name.c_str(), (long)size());
  _st->open = false;
  if (_st->fd >= 0) { ::close(_st->fd); _st->fd = -1; }
  return ok;
}
bool File::seek(uint32_t pos) {
  if (!isOpen() || _st->dir) return false;
  if (pos > size()) return false;
  _st->pos = pos;
  return true;
}
size_t File::getName(char *name, size_t sz) {
  if (!name || !sz) return 0;
  name[0] = 0;
  if (!isOpen()) return 0;
  snprintf(name, sz, "%s", _st->name.c_str());
  return strlen(name);
}
const char *File::name() const { return isOpen() ? _st->name.c_str() : ""; }
bool File::getWriteError() const { return isOpen() ? _st->werr : true; }
void File::clearWriteError() { if (_st) _st->werr = false; Print::clearWriteError(); }
bool File::remove() {
  if (!isOpen() || _st->dir) return false;
  bool ok = ::unlink(_st->path.c_str()) == 0;
  close();
  return ok;
}
bool File::truncate(uint32_t len) {
  if (!isOpen() || _st->dir) return false;
  return ftruncate(_st->fd, len) == 0;
}
void File::rewindDirectory() { if (isDir()) _st->idx = 0; }

static File make_file(const std::string &hp, const std::string &nm, oflag_t of) {
  File f;
  struct stat st;
  bool exists = stat(hp.c_str(), &st) == 0;
  EmuFileState *s = new EmuFileState();
  s->refs = 1; s->fd = -1; s->dir = false; s->open = false; s->werr = false;
  s->writable = (of & O_ACCMODE_EMU) != O_RDONLY;
  s->path = hp; s->name = nm; s->idx = 0; s->pos = 0;
  f._st = s;
  if (exists && S_ISDIR(st.st_mode)) {
    if (s->writable) return f;
    DIR *d = opendir(hp.c_str());
    if (!d) return f;
    while (dirent *e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      s->entries.push_back(e->d_name);
    }
    closedir(d);
    std::sort(s->entries.begin(), s->entries.end());
    s->dir = true;
    s->open = true;
    return f;
  }
  if (!exists && !(of & O_CREAT_EMU)) return f;
  if (exists && (of & O_EXCL_EMU) && (of & O_CREAT_EMU)) return f;
  if (s->writable && emu_env_bool("EMU_SD_READONLY", 0)) return f;
  int fl = s->writable ? O_RDWR : O_RDONLY;
  if (of & O_CREAT_EMU) fl |= O_CREAT;
  if (of & O_TRUNC_EMU) fl |= O_TRUNC;
  s->fd = ::open(hp.c_str(), fl | O_CLOEXEC, 0644);
  if (s->fd < 0) return f;
  s->open = true;
  if (of & O_AT_END_EMU) s->pos = f.size();
  if (s->writable) sdlog(exists ? "open_w" : "create", nm.c_str(), (long)s->pos);
  return f;
}

File File::openNextFile(oflag_t of) {
  File f;
  if (!isDir()) return f;
  if (card_gone()) return f;
  while (_st->idx < _st->entries.size()) {
    std::string nm = _st->entries[_st->idx++];
    File r = make_file(_st->path + "/" + nm, nm, of);
    if (r) return r;
  }
  return f;
}

bool SdFat::begin(uint8_t csPin, uint32_t maxSck) {
  (void)csPin; (void)maxSck;
  sd_pace(2000);
  struct stat st;
  const char *r = sd_root();
  _begun = r && stat(r, &st) == 0 && S_ISDIR(st.st_mode) && !emu_env_bool("EMU_SD_FAIL", 0);
  g_card_ok = _begun;
  g_sderr = _begun ? 0 : 0x01;
  g_ino_set = _begun; if (_begun) g_ino = st.st_ino;
  if (!_begun) { long ms = emu_env_long("EMU_SD_FAIL_BEGIN_MS", 0); if (ms > 0) emu_sleep_ns((uint64_t)ms * 1000000ull); }
  sdlog("begin", r ? r : "", _begun);
  return _begun;
}
uint8_t SdFat::sdErrorCode() { return g_sderr; }
File SdFat::open(const char *path, oflag_t of) {
  if (!_begun || !path) return File();
  if (card_gone()) return File();
  const char *p = path;
  std::string nm = p;
  size_t sl = nm.find_last_of('/');
  if (sl != std::string::npos) nm = nm.substr(sl + 1);
  return make_file(host_path(p), nm, of);
}
bool SdFat::exists(const char *path) {
  if (!_begun || !path) return false;
  if (card_gone()) return false;
  struct stat st;
  return stat(host_path(path).c_str(), &st) == 0;
}
bool SdFat::remove(const char *path) {
  if (!_begun || !path) return false;
  if (emu_env_bool("EMU_SD_READONLY", 0)) return false;
  if (card_gone()) return false;
  bool ok = ::unlink(host_path(path).c_str()) == 0;
  sdlog("remove", path, ok);
  return ok;
}
bool SdFat::rename(const char *a, const char *b) {
  if (!_begun || !a || !b) return false;
  if (card_gone()) return false;
  if (exists(b)) return false;
  bool ok = ::rename(host_path(a).c_str(), host_path(b).c_str()) == 0;
  sdlog("rename", b, ok);
  return ok;
}
bool SdFat::mkdir(const char *path, bool) {
  if (!_begun) return false;
  return ::mkdir(host_path(path).c_str(), 0755) == 0;
}
bool SdFat::rmdir(const char *path) {
  if (!_begun) return false;
  return ::rmdir(host_path(path).c_str()) == 0;
}
