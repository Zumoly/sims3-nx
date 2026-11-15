// An emulator starts the NRO without argv, so the app names its usual place on the SD.
#define REX_SWITCH_FALLBACK_EXE "/switch/Sims3/sims3.nro"
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <rex/assert.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform/env.h>
#include <rex/string.h>

#include <dirent.h>
#include <ftw.h>
#include <libgen.h>
#include <pwd.h>

// macOS off_t is 64-bit with no *64 large-file variants; Linux keeps the
// explicit *64 forms for legacy 32-bit off_t distributions.
#if defined(__APPLE__) || defined(__SWITCH__)
using rex_off64_t = off_t;
#define rex_fseeko64 fseeko
#define rex_ftello64 ftello
#define rex_ftruncate64 ftruncate
#else
using rex_off64_t = off64_t;
#define rex_fseeko64 fseeko64
#define rex_ftello64 ftello64
#define rex_ftruncate64 ftruncate64
#endif

namespace rex {

std::string path_to_utf8(const std::filesystem::path& path) {
  return path.string();
}

std::u16string path_to_utf16(const std::filesystem::path& path) {
  return rex::string::to_utf16(path.string());
}

std::filesystem::path to_path(const std::string_view source) {
  return source;
}

std::filesystem::path to_path(const std::u16string_view source) {
  return rex::string::to_utf8(source);
}

namespace filesystem {

#if defined(__SWITCH__)
// libnx stores the arguments hbloader passes here. They are not declared in its
// headers, but they are libnx.a symbols. C linkage: the namespace does not matter.
extern "C" int __system_argc;
extern "C" char** __system_argv;
#endif

std::filesystem::path GetExecutablePath() {
#if defined(__APPLE__)
  // Darwin has no /proc; query the executable path via the dyld API. The first
  // call reports the required buffer size.
  uint32_t executable_path_size = 0;
  _NSGetExecutablePath(nullptr, &executable_path_size);
  if (!executable_path_size) {
    return {};
  }

  std::string executable_path(executable_path_size, '\0');
  if (_NSGetExecutablePath(executable_path.data(), &executable_path_size) != 0) {
    return {};
  }

  if (!executable_path.empty() && executable_path.back() == '\0') {
    executable_path.pop_back();
  }

  std::error_code ec;
  std::filesystem::path canonical_path = std::filesystem::weakly_canonical(executable_path, ec);
  return ec ? std::filesystem::path(executable_path) : canonical_path;
#elif defined(__SWITCH__)
  // Horizon has no /proc. The Linux branch would compile anyway and silently return
  // empty. hbloader passes the NRO path as argv[0], for example
  // sdmc:/switch/masseffect-nx/masseffect-nx.nro, and libnx stores it in __system_argv.
  //
  // It is returned without the "sdmc:". libstdc++ does not know newlib's devices:
  // to it, "sdmc:/switch/masseffect-nx" has no root, so it is relative, and
  // std::filesystem::absolute prepends the current directory. The result is
  // "sdmc:/switch/masseffect-nx/sdmc:/switch/masseffect-nx/game_root", stat fails with an error
  // other than "does not exist" and exists() throws filesystem_error. That is how
  // A first boot died, in Runtime::SetupVfs, with nothing in the log.
  // "/switch/..." does have a root, and newlib resolves it on the default device,
  // which is the SD. It is only stripped if that form can be queried.
  if (__system_argc > 0 && __system_argv && __system_argv[0] && __system_argv[0][0]) {
    std::string path = __system_argv[0];
    constexpr std::string_view kSdmc = "sdmc:/";
    if (std::string_view(path).starts_with(kSdmc)) {
      std::string rooted = path.substr(kSdmc.size() - 1);
      struct stat st;
      if (stat(rooted.c_str(), &st) == 0) {
        return std::filesystem::path(rooted);
      }
    }
    return std::filesystem::path(path);
  }
#if defined(REX_SWITCH_FALLBACK_EXE)
  // banjo-nx (2026-10-17): an emulator (Ryujinx) starts NROs without argv, so the app would not know its folder
  // (no toml, logs or game_root next to it). The app names its usual place on the SD (the root form, as above),
  // used when it is there. hbloader on a console always passes argv[0].
  {
    struct stat st;
    if (stat(REX_SWITCH_FALLBACK_EXE, &st) == 0) {
      return std::filesystem::path(REX_SWITCH_FALLBACK_EXE);
    }
  }
#endif
  return {};
#else
  char buff[FILENAME_MAX] = "";
  readlink("/proc/self/exe", buff, FILENAME_MAX);
  std::string s(buff);
  return s;
#endif
}

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetUserFolder() {
#if defined(__SWITCH__)
  // Horizon has no XDG_DATA_HOME, no HOME and no user database. The branch below
  // would end up in pw->pw_dir with pw NULL and kill the process at startup:
  // rex_app.cpp calls it to decide the data folder. On Switch the data goes next
  // to the NRO, as in the portable PC version.
  auto exe_dir = GetExecutableFolder();
  return exe_dir.empty() ? std::filesystem::path("/switch") : exe_dir;
#else
  // get preferred data home
  if (auto xdg = rex::platform::env::get("XDG_DATA_HOME")) {
    return std::filesystem::path(*xdg);
  }

  // if XDG_DATA_HOME not set, fallback to HOME directory
  if (auto home = rex::platform::env::get("HOME")) {
    return std::filesystem::path(*home) / ".local" / "share";
  }

  // if HOME not set, fall back to passwd entry
  struct passwd pw1;
  struct passwd* pw;
  char buf[4096];  // could potentionally lower this
  getpwuid_r(getuid(), &pw1, buf, sizeof(buf), &pw);
  assert(&pw1 == pw);  // sanity check
  return std::filesystem::path(pw->pw_dir) / ".local" / "share";
#endif
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return fopen(path.c_str(), std::string(mode).c_str());
}

bool Seek(FILE* file, int64_t offset, int origin) {
  return rex_fseeko64(file, rex_off64_t(offset), origin) == 0;
}

int64_t Tell(FILE* file) {
  return int64_t(rex_ftello64(file));
}

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (fflush(file)) {
    return false;
  }
  int64_t position = Tell(file);
  if (position < 0) {
    return false;
  }
  if (rex_ftruncate64(fileno(file), rex_off64_t(length))) {
    return false;
  }
  if (uint64_t(position) > length) {
    if (!Seek(file, 0, SEEK_END)) {
      return false;
    }
  }
  return true;
}

static int removeCallback(const char* fpath, const struct stat* sb, int typeflag,
                          struct FTW* ftwbuf) {
  int rv = remove(fpath);
  return rv;
}

static uint64_t convertUnixtimeToWinFiletime(time_t unixtime) {
  // Linux uses number of seconds since 1/1/1970, and Windows uses
  // number of nanoseconds since 1/1/1601
  // so we convert linux time to nanoseconds and then add the number of
  // nanoseconds from 1601 to 1970
  // see https://msdn.microsoft.com/en-us/library/ms724228
  uint64_t filetime = filetime = (unixtime * 10000000) + 116444736000000000;
  return filetime;
}

bool CreateEmptyFile(const std::filesystem::path& path) {
  int file = creat(path.c_str(), 0774);
  if (file >= 0) {
    close(file);
    return true;
  }
  return false;
}

#if defined(__SWITCH__)
/*
 * Sims3-NX: one Horizon file per path, however many guest handles it has.
 *
 * On the Xbox 360 a game can hold a file open several times. Horizon refuses a write open while any
 * other open exists, and any open while a write open exists. The Sims 3 creates its save file, keeps
 * two read handles on it and then opens it for writing, which failed with NO_SUCH_FILE: the game left
 * its CORRUPT marker behind and reported that saving failed.
 *
 * So every FileHandle on a path shares one descriptor. It is reopened read-write when the first writer
 * arrives. A per-file lock covers each transfer, since newlib's pread and pwrite move the shared file
 * position. stat also opens the file on Horizon and fails while a writer holds it, so GetInfo asks the
 * shared descriptor first.
 */
struct SharedHostFile {
  std::string key;
  int fd = -1;
  bool writable = false;
  uint32_t references = 0;
  std::mutex mutex;  // taken after g_shared_files_mutex, never before it
};

static std::mutex g_shared_files_mutex;
static std::unordered_map<std::string, std::unique_ptr<SharedHostFile>> g_shared_files;

static bool MakeSharedFileWritable(SharedHostFile& shared) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  if (shared.writable) return true;
  close(shared.fd);
  shared.fd = open(shared.key.c_str(), O_RDWR);
  if (shared.fd == -1) {
    REXFS_ERROR("Could not reopen '{}' for writing: errno {}", shared.key, errno);
    shared.fd = open(shared.key.c_str(), O_RDONLY);
    return false;
  }
  shared.writable = true;
  return true;
}

static bool StatSharedFile(const std::filesystem::path& path, struct stat* st) {
  std::lock_guard<std::mutex> registry(g_shared_files_mutex);
  const auto it = g_shared_files.find(path.string());
  if (it == g_shared_files.end()) return false;
  std::lock_guard<std::mutex> lock(it->second->mutex);
  std::memset(st, 0, sizeof(*st));
  if (fstat(it->second->fd, st) != 0) return false;
  st->st_mode = (st->st_mode & ~S_IFMT) | S_IFREG;
  return true;
}

bool TruncateOpenFile(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> registry(g_shared_files_mutex);
  const auto it = g_shared_files.find(path.string());
  if (it == g_shared_files.end()) return false;
  SharedHostFile& shared = *it->second;
  if (!MakeSharedFileWritable(shared)) return false;
  std::lock_guard<std::mutex> lock(shared.mutex);
  return ftruncate(shared.fd, 0) == 0;
}

class SharedFileHandle : public FileHandle {
 public:
  SharedFileHandle(std::filesystem::path path, SharedHostFile* shared)
      : FileHandle(std::move(path)), shared_(shared) {}
  ~SharedFileHandle() override {
    std::lock_guard<std::mutex> registry(g_shared_files_mutex);
    if (--shared_->references == 0) {
      close(shared_->fd);
      g_shared_files.erase(shared_->key);
    }
  }
  // Host buffer in 1 MB pieces for the reason given on PosixFileHandle's Switch path.
  static constexpr size_t kPiece = size_t(1) << 20;
  static uint8_t* Bounce() {
    static thread_local std::unique_ptr<uint8_t[]> bounce;
    if (!bounce) bounce = std::make_unique<uint8_t[]>(kPiece);
    return bounce.get();
  }
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    uint8_t* const bounce = Bounce();
    auto* out = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < buffer_length) {
      const size_t piece = std::min(kPiece, buffer_length - total);
      ssize_t got;
      {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        got = pread(shared_->fd, bounce, piece, off_t(file_offset + total));
      }
      if (got < 0) {
        if (!total) {
          *out_bytes_read = 0;
          return false;
        }
        break;
      }
      std::memcpy(out + total, bounce, size_t(got));
      total += size_t(got);
      if (size_t(got) < piece) break;
    }
    *out_bytes_read = total;
    return true;
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    uint8_t* const bounce = Bounce();
    const auto* in = static_cast<const uint8_t*>(buffer);
    size_t total = 0;
    while (total < buffer_length) {
      const size_t piece = std::min(kPiece, buffer_length - total);
      std::memcpy(bounce, in + total, piece);
      ssize_t put;
      {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        put = pwrite(shared_->fd, bounce, piece, off_t(file_offset + total));
      }
      if (put < 0) {
        if (!total) {
          *out_bytes_written = 0;
          return false;
        }
        break;
      }
      total += size_t(put);
      if (size_t(put) < piece) break;
    }
    *out_bytes_written = total;
    return true;
  }
  bool SetLength(size_t length) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return ftruncate(shared_->fd, length) >= 0;
  }
  void Flush() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    fsync(shared_->fd);
  }

 private:
  SharedHostFile* shared_;
};
#endif  // __SWITCH__

class PosixFileHandle : public FileHandle {
 public:
  PosixFileHandle(std::filesystem::path path, int handle)
      : FileHandle(std::move(path)), handle_(handle) {}
  ~PosixFileHandle() override {
    close(handle_);
    handle_ = -1;
  }
#if defined(__SWITCH__)
  /*
   * banjo-nx (2026-10-29, for the DLC; from peggle1-nx, 2026-10-21): on Horizon the file system never sees the caller's buffer. The buffers here are usually
   * guest memory, mapped through process code memory and committed page by page on first touch
   * (core/guest_memory_switch.h), which the file system refuses as an IPC transfer buffer (0xD401: the asset pack
   * saw it and reads such pieces through a host buffer, pack_storage_switch.cpp). Peggle's first read of its DLC
   * (content.pak, 22,907,436 bytes in one NtReadFile, straight into a fresh guest allocation) froze the whole console
   * with no log or crash report. So every read and write goes through a host buffer in 1 MB pieces: one memcpy more
   * (about 5 ms for those 23 MB on the A57), the same results as one pread / pwrite (a short piece ends the transfer
   * with what was moved, an error before the first byte is an error).
   */
  static constexpr size_t kPiece = size_t(1) << 20;
  static uint8_t* Bounce() {
    static thread_local std::unique_ptr<uint8_t[]> bounce;
    if (!bounce) bounce = std::make_unique<uint8_t[]>(kPiece);
    return bounce.get();
  }
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    uint8_t* const bounce = Bounce();
    auto* out = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < buffer_length) {
      const size_t piece = std::min(kPiece, buffer_length - total);
      const ssize_t got = pread(handle_, bounce, piece, off_t(file_offset + total));
      if (got < 0) {
        if (!total) {
          *out_bytes_read = 0;
          return false;
        }
        break;
      }
      std::memcpy(out + total, bounce, size_t(got));
      total += size_t(got);
      if (size_t(got) < piece) break;
    }
    *out_bytes_read = total;
    return true;
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    uint8_t* const bounce = Bounce();
    const auto* in = static_cast<const uint8_t*>(buffer);
    size_t total = 0;
    while (total < buffer_length) {
      const size_t piece = std::min(kPiece, buffer_length - total);
      std::memcpy(bounce, in + total, piece);
      const ssize_t put = pwrite(handle_, bounce, piece, off_t(file_offset + total));
      if (put < 0) {
        if (!total) {
          *out_bytes_written = 0;
          return false;
        }
        break;
      }
      total += size_t(put);
      if (size_t(put) < piece) break;
    }
    *out_bytes_written = total;
    return true;
  }
#else
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    ssize_t out = pread(handle_, buffer, buffer_length, file_offset);
    if (out < 0) {
      *out_bytes_read = 0;
      return false;
    }
    *out_bytes_read = static_cast<size_t>(out);
    return true;
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    ssize_t out = pwrite(handle_, buffer, buffer_length, file_offset);
    if (out < 0) {
      *out_bytes_written = 0;
      return false;
    }
    *out_bytes_written = static_cast<size_t>(out);
    return true;
  }
#endif
  bool SetLength(size_t length) override { return ftruncate(handle_, length) >= 0 ? true : false; }
  void Flush() override { fsync(handle_); }

 private:
  int handle_ = -1;
};

std::unique_ptr<FileHandle> FileHandle::OpenExisting(const std::filesystem::path& path,
                                                     uint32_t desired_access,
                                                     bool /*allow_share_delete*/) {
  // POSIX allows unlinking/replacing an open file, so there is no share-delete
  // analog to thread through here.
  // O_RDONLY/O_WRONLY/O_RDWR are an enumeration in the O_ACCMODE bits, not
  // independent flags. kGenericExecute grants neither right, matching
  // GENERIC_EXECUTE on the Windows path.
  constexpr uint32_t kReadRights =
      FileAccess::kGenericRead | FileAccess::kGenericAll | FileAccess::kFileReadData;
  constexpr uint32_t kWriteRights = FileAccess::kGenericWrite | FileAccess::kGenericAll |
                                    FileAccess::kFileWriteData | FileAccess::kFileAppendData;

  const bool want_read = (desired_access & kReadRights) != 0;
  const bool want_write = (desired_access & kWriteRights) != 0;

#if defined(__SWITCH__)
  (void)want_read;
  const std::string key = path.string();
  // The game data is never written, so it keeps one descriptor per handle and no lock. Sharing it
  // stopped the game during startup (log 012): only saves and profiles need the shared path.
  static const std::string game_root = (GetExecutableFolder() / "game").string() + "/";
  if (!want_write && key.rfind(game_root, 0) == 0) {
    const int fd = open(key.c_str(), O_RDONLY);
    if (fd == -1) return nullptr;
    return std::make_unique<PosixFileHandle>(path, fd);
  }
  std::lock_guard<std::mutex> registry(g_shared_files_mutex);
  auto found = g_shared_files.find(key);
  if (found != g_shared_files.end()) {
    if (want_write && !MakeSharedFileWritable(*found->second)) return nullptr;
  } else {
    // Writers always read as well: a later reader on the same path shares this descriptor.
    const int fd = open(key.c_str(), want_write ? O_RDWR : O_RDONLY);
    if (fd == -1) {
      if (errno != ENOENT) REXFS_ERROR("Could not open '{}' (write {}): errno {}", key, want_write, errno);
      return nullptr;
    }
    auto shared = std::make_unique<SharedHostFile>();
    shared->key = key;
    shared->fd = fd;
    shared->writable = want_write;
    found = g_shared_files.emplace(key, std::move(shared)).first;
  }
  ++found->second->references;
  return std::make_unique<SharedFileHandle>(path, found->second.get());
#else
  int open_access = O_RDONLY;
  if (want_read && want_write) {
    open_access = O_RDWR;
  } else if (want_write) {
    open_access = O_WRONLY;
  }

  // No O_APPEND for kFileAppendData: writes go through pwrite with an explicit
  // offset, which O_APPEND would override.
  int handle = open(path.c_str(), open_access);
  if (handle == -1) {
    // TODO(benvanik): pick correct response.
    return nullptr;
  }
  return std::make_unique<PosixFileHandle>(path, handle);
#endif
}

bool GetInfo(const std::filesystem::path& path, FileInfo* out_info) {
  struct stat st;
#if defined(__SWITCH__)
  const bool have_info = StatSharedFile(path, &st) || stat(path.c_str(), &st) == 0;
#else
  const bool have_info = stat(path.c_str(), &st) == 0;
#endif
  if (have_info) {
    if (S_ISDIR(st.st_mode)) {
      out_info->type = FileInfo::Type::kDirectory;
      out_info->total_size = 0;
    } else {
      out_info->type = FileInfo::Type::kFile;
      out_info->total_size = st.st_size;
    }
    out_info->path = path.parent_path();
    out_info->name = path.filename();
    out_info->create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    out_info->access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    out_info->write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    return true;
  }
  return false;
}

std::vector<FileInfo> ListFiles(const std::filesystem::path& path) {
  std::vector<FileInfo> result;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }

  while (auto ent = readdir(dir)) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    FileInfo info;

    info.name = ent->d_name;
    struct stat st;
#if defined(__SWITCH__)
    if (!StatSharedFile(path / info.name, &st) && stat((path / info.name).c_str(), &st) != 0) {
      std::memset(&st, 0, sizeof(st));
    }
#else
    stat((path / info.name).c_str(), &st);
#endif
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    info.path = path;
    if (ent->d_type == DT_DIR) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    result.push_back(info);
  }
  closedir(dir);
  return result;
}

}  // namespace filesystem
}  // namespace rex
