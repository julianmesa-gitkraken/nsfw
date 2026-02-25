#ifndef GITIGNORE_MANAGER_H
#define GITIGNORE_MANAGER_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "GitignoreParser.h"

class GitignoreManager {
public:
  /**
   * Construct a GitignoreManager for the given root watch path.
   *
   * @param rootPath The root directory being watched
   * @param globalGitignorePath Custom path to global gitignore file (empty = use defaults)
   * @param caseSensitive Whether pattern matching is case-sensitive (default: true)
   */
  GitignoreManager(
    const std::string &rootPath,
    const std::string &globalGitignorePath = "",
    bool caseSensitive = true
  );

  /**
   * Returns true if the given path should be ignored.
   * Checks global gitignore, .git/info/exclude, and hierarchical .gitignore rules.
   * Thread-safe.
   *
   * @param fullPath The full/absolute path to check
   * @param isDirectory Whether the path is a directory
   * @return true if the path should be ignored
   */
  bool isIgnored(const std::string &fullPath, bool isDirectory) const;

  /**
   * Returns true if gitignore support is active (.git directory was found).
   */
  bool isActive() const;

  /**
   * Dynamic update: re-read (or add) the .gitignore file in the given directory.
   * Called when a .gitignore file is created or modified at runtime.
   * Thread-safe.
   *
   * @param directoryPath The directory containing the .gitignore file
   */
  void addOrUpdateGitignore(const std::string &directoryPath);

  /**
   * Dynamic update: remove the .gitignore rules for the given directory.
   * Called when a .gitignore file is deleted at runtime.
   * Thread-safe.
   *
   * @param directoryPath The directory that contained the .gitignore file
   */
  void removeGitignore(const std::string &directoryPath);

private:
  void scanForGitignoreFiles(const std::string &dirPath);
  void loadGitignoreFile(const std::string &dirPath);
  void loadGlobalGitignore(const std::string &customPath);
  void loadGitInfoExclude();
  std::string getRelativePath(const std::string &fullPath) const;
  std::string getRelativePathFrom(const std::string &fullPath, const std::string &basePath) const;
  std::string getHomeDirectory() const;
  void checkRules(
    const std::vector<GitignoreRule> &rules,
    const std::string &relativePath,
    bool isDirectory,
    bool &ignored
  ) const;

  std::string mRootPath;
  bool mActive;                    // false if no .git directory found
  bool mCaseSensitive;
  std::vector<GitignoreRule> mGlobalRules;       // From global gitignore
  std::vector<GitignoreRule> mExcludeFileRules;  // From .git/info/exclude
  // Map from directory path to its .gitignore rules
  std::map<std::string, std::vector<GitignoreRule>> mRulesByDirectory;
  mutable std::mutex mMutex;
};

#endif
