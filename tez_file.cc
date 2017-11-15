#include "tez.hh"

#include <assert.h>
#include <zlib.h>

namespace {
  class stored_data_streambuf : public std::streambuf {
    TEZ::archive& tez;
    uint32_t start_pos, cur_pos, end_pos;
    char buffer[4096];
  public:
    stored_data_streambuf(TEZ::archive& tez,
                          uint32_t start_pos, uint32_t end_pos)
      : tez(tez), start_pos(start_pos), cur_pos(start_pos), end_pos(end_pos) {}
    virtual std::streamsize showmanyc() override {
      return end_pos - cur_pos;
    }
    virtual int underflow() override {
      if(cur_pos == end_pos) return std::char_traits<char>::eof();
      uint32_t amount = end_pos - cur_pos;
      if(amount > sizeof(buffer)) amount = sizeof(buffer);
      tez.read_for_file(buffer, cur_pos, amount);
      cur_pos += amount;
      setg(buffer, buffer, buffer + amount);
      return static_cast<unsigned char>(buffer[0]);
    }
    virtual pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                             std::ios_base::openmode which) override {
      assert(which == std::istream::in);
      switch(dir) {
      default:
      case std::istream::beg:
        break;
      case std::istream::cur:
        off += (cur_pos - start_pos);
        break;
      case std::istream::end:
        off += (end_pos - start_pos);
        break;
      }
      return seekpos(off, which);
    }
    virtual pos_type seekpos(pos_type off,
                             std::ios_base::openmode which) override {
      if(off < 0) off = 0;
      else if(off > end_pos - start_pos) off = end_pos - start_pos;
      cur_pos = start_pos + off;
      return off;
    }
  };
  class CRC32 {
    uint32_t crc = crc32(0, nullptr, 0);
  public:
    void update(const uint8_t* buf, size_t len) {
      crc = crc32(crc, buf, len);
    }
    bool check(uint32_t crc) {
      return crc == this->crc;
    }
  };
  class deflated_streambuf : public std::streambuf, private z_stream {
    TEZ::archive& tez;
    uint32_t in_start_pos, in_cur_pos, in_end_pos;
    uint32_t out_cur_pos, out_end_pos, desired_crc;
    CRC32 crc;
    char in_buffer[4096], out_buffer[4096];
  public:
    deflated_streambuf(TEZ::archive& tez,
                       uint32_t start_pos, uint32_t end_pos,
                       uint32_t uncompressed_size, uint32_t desired_crc)
      : tez(tez),
        in_start_pos(start_pos), in_cur_pos(start_pos), in_end_pos(end_pos),
        out_cur_pos(0), out_end_pos(uncompressed_size),
        desired_crc(desired_crc) {
      zalloc = nullptr;
      zfree = nullptr;
      opaque = nullptr;
      if(inflateInit2(this, -15) != Z_OK) {
        throw std::runtime_error("could not initialize zlib");
      }
      avail_in = 0;
      avail_out = 0;
    }
    ~deflated_streambuf() {
      inflateEnd(this);
    }
    virtual std::streamsize showmanyc() override {
      return out_end_pos - out_cur_pos;
    }
    virtual int underflow() override {
      if(out_cur_pos == out_end_pos) return std::char_traits<char>::eof();
      do {
        if(avail_in == 0) {
          uint32_t amount = in_end_pos - in_cur_pos;
          if(amount > sizeof(in_buffer)) amount = sizeof(in_buffer);
          tez.read_for_file(in_buffer, in_cur_pos, amount);
          in_cur_pos += amount;
          next_in = reinterpret_cast<uint8_t*>(in_buffer);
          avail_in = amount;
        }
        next_out = reinterpret_cast<uint8_t*>(out_buffer);
        avail_out = sizeof(out_buffer);
        auto ret = inflate(this, 0);
        if(ret != Z_OK && ret != Z_STREAM_END)
          throw std::runtime_error("zlib error");
      } while(reinterpret_cast<char*>(next_out) == out_buffer);
      setg(out_buffer, out_buffer, reinterpret_cast<char*>(next_out));
      crc.update(reinterpret_cast<uint8_t*>(eback()), egptr()-eback());
      out_cur_pos += egptr() - eback();
      if(out_cur_pos == out_end_pos && !crc.check(desired_crc))
        throw std::runtime_error("checksum mismatch");
      return static_cast<unsigned char>(out_buffer[0]);
    }
    virtual pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                             std::ios_base::openmode which) override {
      assert(which == std::istream::in);
      switch(dir) {
      default:
      case std::istream::beg:
        break;
      case std::istream::cur:
        off += out_cur_pos;
        break;
      case std::istream::end:
        off += out_end_pos;
        break;
      }
      return seekpos(off, which);
    }
    virtual pos_type seekpos(pos_type off,
                             std::ios_base::openmode which) override {
      if(off < 0) off = 0;
      else if(off > out_end_pos) off = out_end_pos;
      if(off < out_cur_pos) {
        // start from the beginning
        inflateReset(this);
        crc = CRC32();
        in_cur_pos = in_start_pos;
        out_cur_pos = 0;
      }
      if(off == out_cur_pos) {
        // don't need to do anything!
        setg(nullptr, nullptr, nullptr);
      }
      else {
        while(underflow() != std::char_traits<char>::eof()
              && out_cur_pos >= off)
          {}
        if(out_cur_pos == out_end_pos) return std::char_traits<char>::eof();
        // the last read region is now *behind* out_cur_pos, let's see how far
        // we overshot by
        auto overshoot_amount = out_cur_pos - off;
        setg(eback(), egptr() - overshoot_amount, egptr());
      }
      return out_cur_pos;
    }
  };
  template<class T> class istream_embedded_buf : public std::istream {
    T buf;
  public:
    template<class... Args> istream_embedded_buf(Args&&... args)
      : std::istream(&buf), buf(args...) {}
  };
}

std::unique_ptr<std::istream> TEZ::file::open(TEZ::archive& tez) const {
  switch(method) {
  default:
  case 0:
    assert(compressed_size == uncompressed_size);
    return std::make_unique<istream_embedded_buf<stored_data_streambuf>>
      (tez, offset, offset+uncompressed_size);
    break;
  case 8:
    return std::make_unique<istream_embedded_buf<deflated_streambuf>>
      (tez, offset, offset+compressed_size, uncompressed_size, crc32);
    break;
  }
  /* NOTREACHED */
}
