#ifndef TEZHH
#define TEZHH

#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// all TEZ functions throw an exception on failure!
namespace TEZ {
  class archive;
  class file {
    uint32_t offset;
    uint32_t crc32, compressed_size, uncompressed_size;
    const std::string* filename; // owned by file_map
    std::unique_ptr<std::string> comment;
    uint16_t method;
    void read_header(archive&, uint32_t);
    friend class archive;
  public:
    const std::string& get_filename() const { return *filename; }
    bool is_directory() const {
      if(!filename->empty()) return *filename->rbegin() == '/';
      else return false; // should never happen
    }
    uint32_t get_crc32() const { return crc32; }
    uint32_t get_compressed_size() const { return compressed_size; }
    uint32_t get_uncompressed_size() const { return uncompressed_size; }
    const std::string& get_comment() {
      if(!comment) comment = std::make_unique<std::string>();
      return *comment;
    }
    std::string purge_comment() {
      if(!comment) return std::string();
      else {
        std::string ret = std::move(*comment);
        comment.reset();
        return ret;
      }
    }
    std::unique_ptr<std::istream> open(archive&) const;
  };
  // you should have only one of these per application, and it should be global
  // (so it doesn't gum up the heap or stack)
  class archive : std::unique_ptr<file[]> {
    friend class file;
    std::mutex mutex;
    std::istream stream;
    uint32_t streampos;
    // if your TEZ archives are big enough that a uint32_t can't hold the file
    // index anymore, you are officially doing something wrong
    uint32_t file_count;
    std::unordered_map<std::string, uint32_t> file_map;
    std::unique_ptr<std::string> comment;
#if defined(WIN32) && defined(__GNUC__)
    __gnu_cxx::stdio_filebuf buf;
#else
    std::filebuf buf;
#endif
    uint32_t read_eocd(ssize_t);
    void read_central_directory(uint32_t);
    using iterator_traits = std::iterator_traits<file*>;
  public:
    void read_for_file(void* buffer, uint32_t offset, uint32_t length);
    typedef file* iterator;
    typedef file* const_iterator;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    // you need to call init!
    archive() : stream(&buf), streampos(0) {}
    // initializes the archive
    void init(const char* argv0);
    // frees all allocated memory for the archive
    void purge();
    const std::string& get_comment() {
      if(!comment) comment = std::make_unique<std::string>();
      return *comment;
    }
    std::string purge_comment() {
      if(!comment) return std::string();
      else {
        std::string ret = std::move(*comment);
        comment.reset();
        return ret;
      }
    }
    // containerish things
    auto size() const { return file_count; }
    bool empty() const { return file_count == 0; }
    auto begin() const { return get(); }
    auto end() const { return get() + file_count; }
    auto cbegin() const { return get(); }
    auto cend() const { return get() + file_count; }
    auto rbegin() const { return reverse_iterator(get() + file_count); }
    auto rend() const { return reverse_iterator(get()); }
    auto crbegin() const { return reverse_iterator(get() + file_count); }
    auto crend() const { return reverse_iterator(get()); }
    auto& operator[](iterator_traits::difference_type index) const {
      return begin()[index];
    }
    auto operator+(iterator_traits::difference_type index) const {
      return begin() + index;
    }
    auto& at(iterator_traits::difference_type index) const {
      if(index < 0 || index >= file_count)
        throw std::out_of_range("file index out of range");
      return begin()[index];
    }
    auto& operator[](const std::string& filename) const {
      auto it = file_map.find(filename);
      if(it == file_map.end())
        throw std::out_of_range("file index out of range");
      return (*this)[it->second];
    }
    auto find(const std::string& filename) const {
      auto it = file_map.find(filename);
      if(it == file_map.end()) return end();
      return begin() + it->second;
    }
  };
}

namespace std {
  template<> class iterator_traits<TEZ::archive::reverse_iterator>
    : public iterator_traits<TEZ::archive::iterator> {};
}

#endif
