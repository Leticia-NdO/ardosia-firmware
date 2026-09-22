#pragma once

// In-memory SD stand-in for host tests of file_manager.cpp.
// Mirrors the subset of the real SDCardManager/FsFile API that file_manager
// actually calls. Not a general SdFat fake.

#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

using oflag_t = int;
constexpr oflag_t O_RDONLY = 0x00;
constexpr oflag_t O_WRONLY = 0x01;
constexpr oflag_t O_CREAT  = 0x02;
constexpr oflag_t O_TRUNC  = 0x04;
constexpr oflag_t O_APPEND = 0x08;

class FsFile {
 public:
  FsFile() = default;
  explicit operator bool() const { return valid_; }
  bool isDirectory() const { return dir_; }
  void close();
  void rewindDirectory();
  FsFile openNextFile();
  void getName(char* name, size_t n) const;
  int read(void* buf, size_t n);
  uint32_t size() const;
  size_t write(const uint8_t* data, size_t n);
  bool getCreateDateTime(uint16_t* pdate, uint16_t* ptime) const;

  // Filled by SDCardManager::open.
  std::string path;
  bool valid_ = false;
  bool dir_ = false;
  oflag_t oflag_ = O_RDONLY;
  size_t pos_ = 0;
  size_t dirIndex_ = 0;
  std::vector<std::string> dirKids_;
};

class SDCardManager {
 public:
  static SDCardManager& getInstance();

  bool begin();
  bool exists(const char* path);
  bool mkdir(const char* path, bool pFlag = true);
  FsFile open(const char* path, oflag_t oflag = O_RDONLY);
  void sleep() {}
  bool remove(const char* path);
  bool rename(const char* path, const char* newPath);

  // Test helpers
  void testReset();
  void testAddFile(const char* path, const std::string& content);
  void testSetCreateDate(const char* path, uint16_t date, uint16_t time);
  std::string testContent(const char* path) const;

  struct Node {
    bool isDir = false;
    std::string data;
    uint16_t createDate = 0;
    uint16_t createTime = 0;
  };
  std::map<std::string, Node> nodes;
};

#define SdMan SDCardManager::getInstance()
