#ifndef INOTIFY_TREE_H
#define INOTIFY_TREE_H
#include <sys/inotify.h>
#include <sys/stat.h>
#include <errno.h>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>

#include "../../includes/GitignoreManager.h"

/**
 * void EmitCreatedEvent(std::string directory, std::string file);
 */
using EmitCreatedEvent = std::function<void(std::string, std::string)>;

class InotifyTree {
public:
  InotifyTree(int inotifyInstance, std::string path, const std::vector<std::string> &excludedPaths,
    bool respectGitignore = true, const std::string &globalGitignorePath = "");

  void addDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent = nullptr);
  std::string getError();
  bool getPath(std::string &out, int wd);
  bool hasErrored();
  bool isRootAlive();
  bool nodeExists(int wd);
  void removeDirectory(int wd);
  void renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName);
  void resyncWatches();
  const std::vector<std::string>& getExcludedPaths() const;
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);
  std::shared_ptr<GitignoreManager> getGitignoreManager() const;

  ~InotifyTree();
private:
  static const int ATTRIBUTES = IN_ATTRIB
                              | IN_CREATE
                              | IN_DELETE
                              | IN_MODIFY
                              | IN_MOVED_FROM
                              | IN_MOVED_TO
                              | IN_DELETE_SELF
                              | IN_ONLYDIR;

  struct WatchNode {
    std::string fullPath;
    ino_t inodeNumber;
  };

  void setError(std::string error);
  bool addDirectoryWatch(const std::string &path, bool includeMoveSelf);
  bool addDirectoryTree(const std::string &rootPath, EmitCreatedEvent emitCreatedEvent);
  void removeDirectoryTreeByWd(int rootWd);
  void removeDirectoryTreeByPath(const std::string &rootPath);
  bool isExcludedPath(const std::string &path) const;
  static bool isSameOrDescendantPath(const std::string &path, const std::string &rootPath);
  static std::string normalizePath(const std::string &path);
  static std::string joinPath(const std::string &parent, const std::string &name);
  static std::string getParentPath(const std::string &filePath);
  static bool getDirectoryStat(const std::string &path, struct stat &file);
  bool existWatchedPath();

  std::string mError;
  const int mInotifyInstance;
  std::unordered_map<int, WatchNode> mWatchByWd;
  std::unordered_map<std::string, int> mWdByPath;
  std::unordered_set<ino_t> mWatchedInodes;
  int mRootWd;
  std::vector<std::string> mExcludedPaths;
  std::unordered_set<std::string> mExcludedPathsSet;
  std::string mWatchedPath;
  std::shared_ptr<GitignoreManager> mGitignoreManager;
};

#endif
