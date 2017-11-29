#include "tez.hh"

#include <sstream>
#include <string.h>
#include <assert.h>
#include <algorithm>

#if defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
  constexpr int END_OF_CENTRAL_DIRECTORY_LEN = 22;
  constexpr int CENTRAL_DIRECTORY_RECORD_LEN = 46;
  constexpr int LOCAL_FILE_HEADER_LEN = 30;
  constexpr uint16_t METHOD_STORE = 0;
  constexpr uint16_t METHOD_DEFLATE = 8;
  inline uint32_t get_uint32(const uint8_t* p) {
    return p[0] |
      (uint32_t(p[1])<<8) |
      (uint32_t(p[2])<<16) |
      (uint32_t(p[3])<<24);
  }
  inline uint16_t get_uint16(const uint8_t* p) {
    return p[0] |
       (uint16_t(p[1])<<8);
  }
}

void TEZ::archive::init(const char* argv0) {
  purge();
#if defined(WIN32)
  /* we can't use argv[0] on Windows for a number of reasons */
  (void)argv0;
  std::wstring self_path;
  self_path.resize(256);
  while(true) {
    DWORD r = GetModuleFileNameW(nullptr, self_path.data(), self_path.size());
    if(r == self_path.size() || (r <= 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
      self_path.resize(self_path.size() << 1);
      continue;
    }
    else if(r <= 0) {
      throw std::system_error(GetLastError(), std::generic_category());
    }
    else {
      self_path.resize(r);
      break;
    }
  }
  auto fd = _wopen(self_path.c_str(), _O_RDONLY);
  if(fd < 0)
    throw std::system_error(errno, std::generic_category());
  buf.open(fd);
#else
  /* assuming a POSIX-like from here */
  /* first try /proc/self/exe! */
  buf.open("/proc/self/exe", std::istream::in | std::istream::binary);
  /* if that didn't work, let's look at argv0 */
  if(!buf.is_open() && argv0 != nullptr) {
    if(strchr(argv0, '/') != nullptr) {
      /* absolute or relative path, hopefully we didn't chdir */
      buf.open(argv0, std::istream::in | std::istream::binary);
    }
    else {
      /* bare exe name, let's search the PATH! YAY! */
      const char* path = getenv("PATH");
      if(path) {
        std::string self_path;
        const char* p;
        while((p = strchr(path, ':')) != nullptr) {
          self_path.assign(path, p);
          self_path += '/';
          self_path += argv0;
          if(buf.open(self_path, std::istream::in | std::istream::binary)
             != nullptr) break;
          path = p+1;
        }
        if(!buf.is_open()) {
          self_path = path;
          self_path += '/';
          self_path += argv0;
          buf.open(self_path, std::istream::in | std::istream::binary);
        }
      }
    }
  }
#endif
  if(!buf.is_open())
    throw std::system_error(errno, std::generic_category());
  try {
    stream.exceptions(std::ifstream::failbit | std::ifstream::badbit | std::ifstream::eofbit);
    stream.seekg(0, std::istream::end);
    auto pos = stream.tellg();
    if(pos < 0 || pos > 0xFFFFFFFF)
      throw std::out_of_range("Zip64 is not implemented and this executable is too large");
    ssize_t file_size = stream.tellg();
    if(file_size != pos)
      throw std::out_of_range("Zip64 is not implemented and this executable is too large");
    auto cd_offset = read_eocd(file_size);
    read_central_directory(cd_offset);
  }
  catch(...) {
    purge();
    throw;
  }
}

void TEZ::archive::purge() {
  file_count = 0;
  reset();
  buf.close();
  file_map.clear();
  comment.reset();
  stream.setstate(std::istream::goodbit);
}

uint32_t TEZ::archive::read_eocd(ssize_t file_size) {
  int max_comment_len = 65535;
  // the position of the longest possible end of central directory record
  auto seek_off = (file_size - max_comment_len) - END_OF_CENTRAL_DIRECTORY_LEN;
  if(seek_off < 0) {
    max_comment_len += seek_off;
    seek_off = 0;
  }
  if(max_comment_len < 0)
    throw std::out_of_range("executable too small to possibly be a zipfile");
  stream.seekg(seek_off);
  ssize_t buf_len = max_comment_len + END_OF_CENTRAL_DIRECTORY_LEN;
  std::unique_ptr<uint8_t[]> buf = std::make_unique<uint8_t[]>(buf_len);
  stream.read(reinterpret_cast<char*>(buf.get()), buf_len);
  assert(stream.gcount() == buf_len);
  int comment_len;
  for(comment_len = 0; comment_len <= max_comment_len; ++comment_len) {
    uint8_t* p = buf.get() + buf_len - comment_len - END_OF_CENTRAL_DIRECTORY_LEN;
    if(get_uint32(p) == 0x06054b50) break; // found it!
  }
  if(comment_len > max_comment_len)
    throw std::runtime_error("executable does not appear to contain a zipfile");
  uint8_t* p = buf.get() + buf_len - comment_len - END_OF_CENTRAL_DIRECTORY_LEN;
  if(std::find_if(p+4, p+8, [](uint8_t p) { return p != 0; }) != p+8) {
    throw std::out_of_range("multipart zipfiles are not supported");
  }
  // ignore the file count "on this disk"
  file_count = get_uint16(p+10);
  // allocate the needed number of entries
  *static_cast<std::unique_ptr<file[]>*>(this)
    = std::move(std::make_unique<file[]>(file_count));
  // ignore central directory size
  uint32_t cd_offset = get_uint32(p+16);
  uint16_t comment_length = get_uint16(p+20);
  comment = std::make_unique<std::string>(reinterpret_cast<char*>(p+22),
                                          reinterpret_cast<char*>(p+22+comment_length));
  return cd_offset;
}

void TEZ::archive::read_central_directory(uint32_t cd_offset) {
  stream.seekg(cd_offset);
  std::unique_ptr<std::string[]> filenames = std::make_unique<std::string[]>(file_count);
  // fileno can't overflow, file_count will not be >65535
  for(uint32_t fileno = 0; fileno < file_count; ++fileno) {
    auto& file = get()[fileno];
    uint8_t buf[CENTRAL_DIRECTORY_RECORD_LEN];
    stream.read(reinterpret_cast<char*>(buf), CENTRAL_DIRECTORY_RECORD_LEN);
    assert(stream.gcount() == CENTRAL_DIRECTORY_RECORD_LEN);
    if(get_uint32(buf) != 0x02014b50)
      throw std::runtime_error("central directory is corrupted");
    // ignore version made by
    // version needed to extract
    if(get_uint16(buf+6) > 20)
      throw std::out_of_range("zipfile is not PKZIP 2.0 compatible");
    uint16_t general_bitflag = get_uint16(buf+8);
    if(general_bitflag & 1)
      throw std::out_of_range("zipfile contains an encrypted member");
    else if(general_bitflag & 8)
      throw std::out_of_range("zipfile uses Data Descriptors");
    else if(general_bitflag & 0xF7F0)
      throw std::out_of_range("zipfile member uses unsupported GPBF flags");
    file.method = get_uint16(buf+10);
    if(file.method != METHOD_STORE && file.method != METHOD_DEFLATE)
      throw std::out_of_range("zipfile uses a compression method other than deflate");
    // skip modification time and date
    file.crc32 = get_uint32(buf+16);
    file.compressed_size = get_uint32(buf+20);
    file.uncompressed_size = get_uint32(buf+24);
    uint16_t filename_length = get_uint16(buf+28);
    uint16_t extra_length = get_uint16(buf+30);
    uint16_t comment_length = get_uint16(buf+32);
    uint16_t disk_number = get_uint16(buf+34);
    if(disk_number > 0)
      throw std::out_of_range("multipart zipfiles are not supported");
    // ignore internal and external file attributes
    file.offset = get_uint32(buf+42);
    auto& filename = filenames[fileno];
    filename.resize(filename_length);
    stream.read(const_cast<char*>(filename.data()), filename_length);
    assert(stream.gcount() == filename_length);
    stream.seekg(extra_length, std::istream::cur);
    comment = std::make_unique<std::string>();
    comment->resize(comment_length);
    stream.read(const_cast<char*>(comment->data()), comment_length);
    assert(stream.gcount() == comment_length);
  }
  file_map.reserve(file_count);
  for(uint32_t fileno = 0; fileno < file_count; ++fileno) {
    file_map.emplace(std::move(filenames[fileno]), fileno);
  }
  for(auto it = file_map.begin(); it != file_map.end(); ++it) {
    (*this)[it->second].filename = &it->first;
  }
  for(auto it = begin(); it != end(); ++it) {
    it->read_header(*this, it->offset);
  }
}

void TEZ::archive::read_for_file(void* _buffer,
                                 uint32_t offset, uint32_t length) {
  std::unique_lock<std::mutex> lock(mutex);
  char* buffer = reinterpret_cast<char*>(_buffer);
  if(streampos != offset)
    stream.seekg(offset);
  stream.read(buffer, length);
  assert(stream.gcount() == length);
  streampos = offset + length;
}

// technically not a member of archive, but this is really where it belongs
void TEZ::file::read_header(archive& tez, uint32_t offset) {
  tez.stream.seekg(offset);
  uint8_t buf[LOCAL_FILE_HEADER_LEN];
  tez.stream.read(reinterpret_cast<char*>(buf), LOCAL_FILE_HEADER_LEN);
  assert(tez.stream.gcount() == LOCAL_FILE_HEADER_LEN);
  if(get_uint32(buf) != 0x04034b50)
    throw std::runtime_error("file header is corrupted");
  // ignore most of the headers! we trust the central directory!
  uint16_t filename_length = get_uint16(buf+26);
  uint16_t extra_length = get_uint16(buf+28);
  this->offset = offset + LOCAL_FILE_HEADER_LEN + filename_length + extra_length;
}
