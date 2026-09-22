#include "SDCardManager.h"

#include <cstring>

static std::string norm(const char* path) {
  if (path == nullptr || path[0] == '\0') return "/";
  std::string s(path);
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

static std::string parentDir(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

static std::string baseName(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

SDCardManager& SDCardManager::getInstance() {
  static SDCardManager inst;
  return inst;
}

bool SDCardManager::begin() { return true; }

bool SDCardManager::exists(const char* path) {
  return nodes.find(norm(path)) != nodes.end();
}

bool SDCardManager::mkdir(const char* path, bool /*pFlag*/) {
  const std::string p = norm(path);
  Node& n = nodes[p];
  n.isDir = true;
  n.data.clear();
  return true;
}

FsFile SDCardManager::open(const char* path, oflag_t oflag) {
  FsFile f;
  const std::string p = norm(path);
  auto it = nodes.find(p);
  const bool wantWrite = (oflag & O_WRONLY) != 0;
  const bool wantCreate = (oflag & O_CREAT) != 0;
  const bool wantTrunc = (oflag & O_TRUNC) != 0;
  const bool wantAppend = (oflag & O_APPEND) != 0;

  if (it == nodes.end()) {
    if (!wantCreate) return f;
    Node n;
    n.isDir = false;
    nodes[p] = n;
    it = nodes.find(p);
  }

  f.path = p;
  f.valid_ = true;
  f.dir_ = it->second.isDir;
  f.oflag_ = oflag;
  f.pos_ = 0;

  if (f.dir_) {
    f.rewindDirectory();
    return f;
  }

  if (wantWrite && wantTrunc) it->second.data.clear();
  if (wantAppend) f.pos_ = it->second.data.size();
  return f;
}

bool SDCardManager::remove(const char* path) {
  return nodes.erase(norm(path)) > 0;
}

bool SDCardManager::rename(const char* path, const char* newPath) {
  const std::string from = norm(path);
  const std::string to = norm(newPath);
  auto it = nodes.find(from);
  if (it == nodes.end()) return false;
  nodes[to] = it->second;
  nodes.erase(it);
  return true;
}

void SDCardManager::testReset() { nodes.clear(); }

void SDCardManager::testSetCreateDate(const char* path, uint16_t date, uint16_t time) {
  auto it = nodes.find(norm(path));
  if (it == nodes.end()) return;
  it->second.createDate = date;
  it->second.createTime = time;
}

void SDCardManager::testAddFile(const char* path, const std::string& content) {
  const std::string p = norm(path);
  Node n;
  n.isDir = false;
  n.data = content;
  nodes[p] = n;
  const std::string dir = parentDir(p);
  if (nodes.find(dir) == nodes.end()) {
    Node d;
    d.isDir = true;
    nodes[dir] = d;
  }
}

std::string SDCardManager::testContent(const char* path) const {
  auto it = nodes.find(norm(path));
  if (it == nodes.end()) return {};
  return it->second.data;
}

void FsFile::close() { valid_ = false; }

void FsFile::rewindDirectory() {
  dirIndex_ = 0;
  dirKids_.clear();
  if (!dir_) return;
  for (const auto& kv : SdMan.nodes) {
    if (kv.first == path) continue;
    if (parentDir(kv.first) == path) {
      dirKids_.push_back(baseName(kv.first));
    }
  }
}

FsFile FsFile::openNextFile() {
  FsFile child;
  if (!dir_ || dirIndex_ >= dirKids_.size()) return child;
  const std::string childPath = (path == "/") ? "/" + dirKids_[dirIndex_]
                                              : path + "/" + dirKids_[dirIndex_];
  dirIndex_++;
  return SdMan.open(childPath.c_str(), O_RDONLY);
}

void FsFile::getName(char* name, size_t n) const {
  if (name == nullptr || n == 0) return;
  const std::string b = baseName(path);
  strncpy(name, b.c_str(), n - 1);
  name[n - 1] = '\0';
}

int FsFile::read(void* buf, size_t n) {
  if (!valid_ || buf == nullptr) return -1;
  auto it = SdMan.nodes.find(path);
  if (it == SdMan.nodes.end()) return -1;
  const std::string& data = it->second.data;
  if (pos_ >= data.size()) return 0;
  const size_t remain = data.size() - pos_;
  const size_t take = remain < n ? remain : n;
  memcpy(buf, data.data() + pos_, take);
  pos_ += take;
  return static_cast<int>(take);
}

uint32_t FsFile::size() const {
  auto it = SdMan.nodes.find(path);
  if (it == SdMan.nodes.end()) return 0;
  return static_cast<uint32_t>(it->second.data.size());
}

bool FsFile::getCreateDateTime(uint16_t* pdate, uint16_t* ptime) const {
  auto it = SdMan.nodes.find(path);
  if (it == SdMan.nodes.end()) return false;
  if (it->second.createDate == 0 && it->second.createTime == 0) return false;
  if (pdate) *pdate = it->second.createDate;
  if (ptime) *ptime = it->second.createTime;
  return true;
}

size_t FsFile::write(const uint8_t* data, size_t n) {
  if (!valid_ || data == nullptr) return 0;
  auto it = SdMan.nodes.find(path);
  if (it == SdMan.nodes.end()) return 0;
  std::string& body = it->second.data;
  if (pos_ > body.size()) pos_ = body.size();
  if (pos_ + n > body.size()) body.resize(pos_ + n);
  memcpy(&body[pos_], data, n);
  pos_ += n;
  return n;
}
