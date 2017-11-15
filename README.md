This is a C++14 library for embedding data files in your executable. It's pretty easy to use, and vaguely portable. (The Windows code is not tested, but if it compiles it probably works.)

# Rationale

Sometimes you want to include certain data with your application, without which it cannot function. Internationalization data, for instance, or giant lookup tables. Most applications can be distributed with installers, big "Data" directories, and the like... but when they can't be, this data is often either embedded with platform-specific systems (e.g. Bundles on macOS), or compiled in as big arrays. The former is non-portable, and the latter can waste a lot of memory or disk space (or both). TEZ provides an alternative.

TEZ could also be used to make a custom installer, though it isn't directly intended for this purpose.

# License

TEZ is distributed under the zlib license. The (very brief) text of the license can be found in [`LICENSE.md`](LICENSE.md).

# Building

TEZ is designed to be embedded directly in other applications. There is no provision for building it as a standalone library.

Include all `.cc` files in your build. TEZ makes use of C++14 features. Most compilers must be specially instructed to compile in C++14 mode. For gcc/clang, pass `-std=c++14`.

When your program's build is complete, append a zipfile to it, and run something to fix the offsets. An example of this process on a UNIX system:

```sh
g++ -std=c++14 tez/*.cc my_program.cc -o my_program
cat data.zip >> my_program
zip --adjust-sfx my_program
```

# Usage

Include `tez.hh`.

Keep an instance of `TEZ::archive` somewhere. It *should* be a global variable, so it lives in BSS, but it could also be a heap- or stack-allocated variable. You can have more than one, but you usually won't need to. Conventionally, this variable is called `tez`.

In `main`, as early as possible, call `tez.init(argv[0])`. If you don't do this, `tez` will not be able to access any files.

Nearly all TEZ methods, including `init`, throw exceptions on failure.

`TEZ::archive` acts like a container of `TEZ::file`s. It has `begin()`/`end()`/`size()`/`operator[]` like you expect from a container. Files are presented in the same order as they exist in the archive. `operator[]` can also access a file by name, throwing an exception if the file wasn't found.

`TEZ::file` represents a single file in the archive. You can call `file.get_filename()` to get its filename, or `file.open(tez)` to get a `std::unique_ptr<std::istream>` you can use to read the file's contents.

TEZ is thread safe; as long as you don't access the same *specific `std::istream`* from more than one thread at a time, you can open and read as many files as you like from as many threads as you like.

Say you have the following directory structure:

- Language
    - English
        - splash.png
        - credits.txt
    - Japanese
        - splash.png
        - credits.txt
    - Russian
        - splash.png
        - credits.txt
- Tables
    - curve_precache.dat

The `TEZ::archive`'s view of this tree might look like:

```
Language/
Language/English/
Language/English/splash.png
Language/English/credits.txt
Language/Japanese/
Language/Japanese/splash.png
Language/Japanese/credits.txt
Language/Russian/
Language/Russian/splash.png
Language/Russian/credits.txt
Tables/
Tables/curve_precache.dat
```

What order the files are in, and whether directories (the paths ending with /) are even present at all, will depend on the order they're present in the archive's central directory. This, in turn, depends on the tool you used to create the archive, and how you used it. In general, files are present in the order in which they were added, and directories are present before the files/directories that they contain.

## Example code

This program uses TEZ to store a splash screen:

```c++
#include "tez.hh"

TEZ::archive tez;

extern void display_splash_screen(std::istream splash_screen_data_stream);

int main(int argc, char* argv[]) {
    tez.init(argv[0]);
    TEZ::file& file = tez["splash_screen.png"];
    std::unique_ptr<std::istream> stream_ptr = file.open(tez);
    display_splash_screen(*stream_ptr);
    // ...
    return 0;
}
```

And this one uses TEZ in combination with [`libsn`](https://github.com/SolraBizna/libsn) for internationalization:

```c++
#include "tez.hh"
#include "sn.hh"

#include <iostream>

TEZ::archive tez;
SN::Context sn;

class TEZCatSource : public SN::CatSource {
public:
  virtual void GetAvailableCats(std::function<void(std::string)> callback) override {
    for(auto&& f : tez) {
      if(f.is_directory()) continue;
      auto& filename = f.get_filename();
      if(filename.length() <= 10) continue;
      if(filename.compare(0, 5, "lang/") != 0) continue;
      if(filename.compare(filename.length()-5, filename.length(), ".utxt") != 0) continue;
      std::string code(filename.begin()+5, filename.end()-5);
      for(auto& c : code) {
        if(c == '_') c = '-';
      }
      if(SN::IsValidLanguageCode(code)) callback(std::move(code));
    }
  }
  virtual std::unique_ptr<std::istream> OpenCat(const std::string& cat) override {
    std::string code(cat);
    for(auto& c : code) {
      if(c == '-') c = '_';
    }
    std::string path;
    path.reserve(10 + code.length());
    path += "lang/";
    path += code;
    path += ".utxt";
    auto found = tez.find(path);
    if(found == tez.end()) return nullptr;
    else return found->open(tez);
  }
};

int main(int argc, char* argv[]) {
    tez.init(argv[0]);
    sn.SetLanguage(sn.GetSystemLanguage());
    sn.AddCatSource(std::make_unique<TEZCatSource>());
    sn.Out(std::cout, "STARTUP_MESSAGE"_Key);
    // ...
    return 0;
}
```

# API

Unless otherwise noted, all methods throw exceptions on failure.

## `TEZ::archive`

An instance of `TEZ::archive` contains information about the files present in the embedded archive. It has the following public methods:

```c++
void init(const char* argv0);
```

Attempts to open the executable and read the embedded zipfile's central directory. On Windows, this uses `GetModuleFileName` to find the EXE. On other OSes, it tries `/proc/self/exe` if that exists, and searches based on `argv[0]` otherwise.

Call this method once, preferably as early in `main()` as possible.

```c++
const std::string& get_comment();
```

Returns the comment for the archive as a whole, or a reference to an empty string if the comment has been purged.

```c++
std::string purge_comment();
```

Returns the comment for the archive as a whole, freeing TEZ's internal storage for it in the process.

```c++
uint32_t size() const;
bool empty() const;
iterator begin() const;
iterator end() const;
const_iterator cbegin() const;
const_iterator cend() const;
reverse_iterator rbegin() const;
reverse_iterator rend() const;
const_reverse_iterator crbegin() const;
const_reverse_iterator crend() const;
TEZ::file& operator[](index) const;
TEZ::file& at(index) const;
iterator operator+(index) const;
TEZ::file& operator[](const std::string& filename) const;
iterator find(const std::string& filename) const;
```

These methods all work as you would expect for an STL container that contains `TEZ::file`s. `find` returns `end()` if the file was not found, `operator[]` on an int will crash if you are careless, and all others throw exceptions as needed. All iterators are random access iterators.

The container contains `TEZ::file`s in the order that they are present in the archive. It is a "flat" view; no extra logic is needed to descend into subdirectories. Files within a directory will *usually* be preceded by `TEZ::file`s for each containing directory, but a zip archive need not even contain directory entries at all. 

## `TEZ::file`

Instances of `TEZ::file` contain information about a single file within the archive. They have the following public methods:

```c++
const std::string& get_filename() const;
```

Returns this file's name.

```c++
bool is_directory() const;
```

Returns `true` if the "file" is actually a directory.

```c++
uint32_t get_crc32() const;
```

Returns the CRC32 checksum of the uncompressed data. If the file was compressed (rather than stored), TEZ checks this for you; if the file fails the check, TEZ reports an IO error on the last read from the file.

```c++
uint32_t get_compressed_size() const;
```

Returns the number of bytes this file's data occupies on disk.

```c++
uint32_t get_uncompressed_size() const;
```

Returns the number of bytes of data this file contains.

```c++
const std::string& get_comment();
```

Returns the comment for this file, or a reference to an empty string if the comment has been purged.

```c++
std::string purge_comment();
```

Returns the comment for this file, freeing TEZ's internal storage for it in the process.

```c++
std::unique_ptr<std::istream> open(TEZ::archive&) const;
```

Returns a unique pointer to a new `std::istream` through which you can read the file's data. The stream is seekable, but will be extremely slow when seeking within compressed files.

The returned `istream` will not, by default, throw exceptions on errors. This is in keeping with standard behavior for newly-created `istream`s. Consider calling `my_stream.exceptions(std::istream::badbit | std::istream::failbit)`.

# Missing / Planned features

- Zip64 support
- Compression methods other than Deflate
