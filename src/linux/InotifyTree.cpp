#include "../../includes/linux/InotifyTree.h"

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

InotifyTree::InotifyTree(int inotifyInstance, std::string path, const std::vector<std::string> &excludedPaths):
  mError(""),
  mInotifyInstance(inotifyInstance),
  mRootWd(-1) {
  mWatchedPath = normalizePath(path);

  mExcludedPaths.reserve(excludedPaths.size());
  mExcludedPathsSet.reserve(excludedPaths.size());
  for (const std::string &excludedPath : excludedPaths) {
    const std::string normalizedPath = normalizePath(excludedPath);
    mExcludedPaths.push_back(normalizedPath);
    mExcludedPathsSet.insert(normalizedPath);
  }

  struct stat file;
  if (!getDirectoryStat(mWatchedPath, file)) {
    return;
  }

  if (!addDirectoryWatch(mWatchedPath, true)) {
    return;
  }

  if (!addDirectoryTree(mWatchedPath, nullptr)) {
    return;
  }
}

bool InotifyTree::addDirectoryWatch(const std::string &path, bool includeMoveSelf) {
  if (mWdByPath.find(path) != mWdByPath.end()) {
    return true;
  }

  struct stat file;
  if (!getDirectoryStat(path, file)) {
    return false;
  }

  if (mWatchedInodes.find(file.st_ino) != mWatchedInodes.end()) {
    return true;
  }

  int attr = ATTRIBUTES;
  if (includeMoveSelf) {
    attr |= IN_MOVE_SELF;
  }

  int watchDescriptor = inotify_add_watch(
    mInotifyInstance,
    path.c_str(),
    attr
  );

  if (watchDescriptor == -1) {
    if (errno == ENOSPC) {
      setError("Inotify limit reached");
    } else if (errno == ENOMEM) {
      setError("Kernel out of memory");
    } else if (errno == EBADF || errno == EINVAL) {
      setError("Invalid file descriptor");
    }
    return false;
  }

  WatchNode node;
  node.fullPath = path;
  node.inodeNumber = file.st_ino;

  mWatchByWd.insert(std::pair<int, WatchNode>(watchDescriptor, node));
  mWdByPath.insert(std::pair<std::string, int>(path, watchDescriptor));
  mWatchedInodes.insert(file.st_ino);

  if (includeMoveSelf) {
    mRootWd = watchDescriptor;
  }

  return true;
}

bool InotifyTree::addDirectoryTree(const std::string &rootPath, EmitCreatedEvent emitCreatedEvent) {
  if (hasErrored()) {
    return false;
  }

  const fs::directory_options options = fs::directory_options::skip_permission_denied;

  try {
    for (fs::recursive_directory_iterator iterator(rootPath, options), end; iterator != end; ++iterator) {
      std::error_code error;
      const fs::directory_entry &entry = *iterator;
      const bool isDirectory = entry.is_directory(error);
      if (error) {
        continue;
      }

      const std::string entryPath = normalizePath(entry.path().string());

      if (isExcludedPath(entryPath)) {
        if (isDirectory) {
          iterator.disable_recursion_pending();
        }
        continue;
      }

      if (emitCreatedEvent) {
        emitCreatedEvent(
          normalizePath(entry.path().parent_path().string()),
          entry.path().filename().string()
        );
      }

      if (!isDirectory) {
        continue;
      }

      if (!addDirectoryWatch(entryPath, false) && hasErrored()) {
        return false;
      }
    }
  } catch (const fs::filesystem_error &) {
    return true;
  }

  return true;
}

bool InotifyTree::existWatchedPath() {
  struct stat file;
  return getDirectoryStat(mWatchedPath, file);
}

void InotifyTree::addDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent) {
  auto watchedNode = mWatchByWd.find(wd);
  if (watchedNode == mWatchByWd.end()) {
    return;
  }

  const std::string childPath = joinPath(watchedNode->second.fullPath, name);
  if (isExcludedPath(childPath)) {
    return;
  }

  if (!addDirectoryWatch(childPath, false)) {
    return;
  }

  addDirectoryTree(childPath, emitCreatedEvent);
}

std::string InotifyTree::getError() {
  return mError;
}

bool InotifyTree::getPath(std::string &out, int wd) {
  auto watchedNode = mWatchByWd.find(wd);
  if (watchedNode == mWatchByWd.end()) {
    return false;
  }

  out = watchedNode->second.fullPath;
  return true;
}

const std::vector<std::string>& InotifyTree::getExcludedPaths() const {
  return mExcludedPaths;
}

bool InotifyTree::hasErrored() {
  if (mError.empty() && !existWatchedPath()) {
    mError = "Service shutdown: root path changed (renamed or deleted)";
  }
  return !mError.empty();
}

bool InotifyTree::isRootAlive() {
  return mRootWd >= 0 && mWatchByWd.find(mRootWd) != mWatchByWd.end();
}

bool InotifyTree::nodeExists(int wd) {
  return mWatchByWd.find(wd) != mWatchByWd.end();
}

void InotifyTree::removeDirectoryTreeByWd(int rootWd) {
  auto watchedNode = mWatchByWd.find(rootWd);
  if (watchedNode == mWatchByWd.end()) {
    return;
  }

  removeDirectoryTreeByPath(watchedNode->second.fullPath);
}

void InotifyTree::removeDirectoryTreeByPath(const std::string &rootPath) {
  std::vector<int> descriptorsToRemove;
  descriptorsToRemove.reserve(mWatchByWd.size());

  for (const auto &pair : mWatchByWd) {
    if (isSameOrDescendantPath(pair.second.fullPath, rootPath)) {
      descriptorsToRemove.push_back(pair.first);
    }
  }

  for (int wd : descriptorsToRemove) {
    auto node = mWatchByWd.find(wd);
    if (node == mWatchByWd.end()) {
      continue;
    }

    mWdByPath.erase(node->second.fullPath);
    mWatchedInodes.erase(node->second.inodeNumber);
    inotify_rm_watch(mInotifyInstance, wd);
    mWatchByWd.erase(node);
  }
}

void InotifyTree::removeDirectory(int wd) {
  auto watchedNode = mWatchByWd.find(wd);
  if (watchedNode == mWatchByWd.end()) {
    return;
  }

  const bool isRoot = wd == mRootWd;
  removeDirectoryTreeByWd(wd);

  if (isRoot) {
    mRootWd = -1;
    setError("Service shutdown: root path changed (renamed or deleted)");
  }
}

void InotifyTree::renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName) {
  std::string fromPath;
  std::string toPath;
  if (!getPath(fromPath, fromWd) || !getPath(toPath, toWd)) {
    return;
  }

  const std::string oldRootPath = joinPath(fromPath, fromName);
  const std::string newRootPath = joinPath(toPath, toName);

  std::vector<std::pair<int, std::string>> updates;
  updates.reserve(mWatchByWd.size());

  for (const auto &pair : mWatchByWd) {
    const std::string &path = pair.second.fullPath;
    if (!isSameOrDescendantPath(path, oldRootPath)) {
      continue;
    }

    std::string newPath;
    if (path == oldRootPath) {
      newPath = newRootPath;
    } else {
      newPath = newRootPath + path.substr(oldRootPath.size());
    }
    updates.push_back(std::pair<int, std::string>(pair.first, std::move(newPath)));
  }

  for (const auto &update : updates) {
    auto node = mWatchByWd.find(update.first);
    if (node != mWatchByWd.end()) {
      mWdByPath.erase(node->second.fullPath);
    }
  }

  for (const auto &update : updates) {
    auto node = mWatchByWd.find(update.first);
    if (node == mWatchByWd.end()) {
      continue;
    }

    node->second.fullPath = update.second;
    mWdByPath[update.second] = update.first;
  }
}

void InotifyTree::setError(std::string error) {
  mError = error;
}

InotifyTree::~InotifyTree() {
  if (mRootWd >= 0) {
    removeDirectoryTreeByWd(mRootWd);
    mRootWd = -1;
  }
}

void InotifyTree::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  std::unordered_set<std::string> newExcludedPaths;
  newExcludedPaths.reserve(excludedPaths.size());

  std::vector<std::string> addedExcludedPaths;
  std::vector<std::string> removedExcludedPaths;

  for (const std::string &excludedPathRaw : excludedPaths) {
    const std::string excludedPath = normalizePath(excludedPathRaw);
    newExcludedPaths.insert(excludedPath);
    if (mExcludedPathsSet.find(excludedPath) == mExcludedPathsSet.end()) {
      addedExcludedPaths.push_back(excludedPath);
    }
  }

  for (const std::string &excludedPath : mExcludedPathsSet) {
    if (newExcludedPaths.find(excludedPath) == newExcludedPaths.end()) {
      removedExcludedPaths.push_back(excludedPath);
    }
  }

  mExcludedPaths.clear();
  mExcludedPaths.reserve(newExcludedPaths.size());
  for (const auto &excludedPath : newExcludedPaths) {
    mExcludedPaths.push_back(excludedPath);
  }
  mExcludedPathsSet = std::move(newExcludedPaths);

  for (const std::string &excludedPath : addedExcludedPaths) {
    removeDirectoryTreeByPath(excludedPath);
  }

  for (const std::string &excludedPath : removedExcludedPaths) {
    const std::string parentPath = getParentPath(excludedPath);
    if (mWdByPath.find(parentPath) == mWdByPath.end()) {
      continue;
    }

    if (!addDirectoryWatch(excludedPath, false)) {
      continue;
    }

    addDirectoryTree(excludedPath, nullptr);
  }
}

bool InotifyTree::isExcludedPath(const std::string &path) const {
  return mExcludedPathsSet.find(path) != mExcludedPathsSet.end();
}

bool InotifyTree::isSameOrDescendantPath(const std::string &path, const std::string &rootPath) {
  if (path == rootPath) {
    return true;
  }

  if (path.size() <= rootPath.size()) {
    return false;
  }

  if (path.compare(0, rootPath.size(), rootPath) != 0) {
    return false;
  }

  return path[rootPath.size()] == '/';
}

std::string InotifyTree::normalizePath(const std::string &path) {
  if (path.empty()) {
    return path;
  }

  std::string output = path;
  while (output.size() > 1 && output.back() == '/') {
    output.pop_back();
  }

  return output;
}

std::string InotifyTree::joinPath(const std::string &parent, const std::string &name) {
  if (name.empty()) {
    return parent;
  }

  if (parent.empty() || parent == "/") {
    return std::string("/") + name;
  }

  return parent + "/" + name;
}

std::string InotifyTree::getParentPath(const std::string &filePath) {
  fs::path path(filePath);
  fs::path parent = path.parent_path();
  if (parent.empty()) {
    return "/";
  }
  return normalizePath(parent.string());
}

bool InotifyTree::getDirectoryStat(const std::string &path, struct stat &file) {
  return stat(path.c_str(), &file) >= 0 && S_ISDIR(file.st_mode);
}
