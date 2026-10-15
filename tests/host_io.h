// stdio-backed Reader/Writer for host builds (tests and the CLI).
#pragma once

#include <cstdio>

#include "../src/engine/wav.h"

namespace mbb
{
class FileReader : public Reader
{
  public:
    explicit FileReader(const char* path) { f_ = std::fopen(path, "rb"); }
    ~FileReader() override
    {
        if(f_)
            std::fclose(f_);
    }
    bool     ok() const { return f_ != nullptr; }
    uint32_t Read(void* dst, uint32_t bytes) override
    {
        return f_ ? static_cast<uint32_t>(std::fread(dst, 1, bytes, f_)) : 0;
    }
    bool Seek(uint32_t pos) override { return f_ && std::fseek(f_, pos, SEEK_SET) == 0; }

  private:
    FILE* f_ = nullptr;
};

class FileWriter : public Writer
{
  public:
    explicit FileWriter(const char* path) { f_ = std::fopen(path, "wb"); }
    ~FileWriter() override
    {
        if(f_)
            std::fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }
    bool Write(const void* src, uint32_t bytes) override
    {
        return f_ && std::fwrite(src, 1, bytes, f_) == bytes;
    }
    bool Seek(uint32_t pos) override { return f_ && std::fseek(f_, pos, SEEK_SET) == 0; }

  private:
    FILE* f_ = nullptr;
};

} // namespace mbb
