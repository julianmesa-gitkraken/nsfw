#include "../includes/GitignoreManager.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#endif

namespace {

bool directoryExists(const std::string &path) {
#if defined(_WIN32)
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool fileExists(const std::string &path) {
#if defined(_WIN32)
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

std::string readFileContents(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::string normalizePath(const std::string &path) {
  std::string result = path;
  // Ensure no trailing slash (unless it's the root "/")
  while (result.size() > 1 && result.back() == '/') {
    result.pop_back();
  }
#if defined(_WIN32)
  // Normalize backslashes to forward slashes
  std::replace(result.begin(), result.end(), '\\', '/');
  while (result.size() > 1 && result.back() == '/') {
    result.pop_back();
  }
#endif
  return result;
}

} // anonymous namespace

GitignoreManager::GitignoreManager(
  const std::string &rootPath,
  const std::string &globalGitignorePath,
  bool caseSensitive
) : mRootPath(normalizePath(rootPath)),
    mActive(false),
    mCaseSensitive(caseSensitive)
{
  // NOTE: To enable gitignore support without a .git directory, remove this check.
  std::string gitDir = mRootPath + "/.git";
  if (!directoryExists(gitDir)) {
    return;
  }

  mActive = true;

  // Load global gitignore rules
  loadGlobalGitignore(globalGitignorePath);

  // Load .git/info/exclude rules
  loadGitInfoExclude();

  // Scan for .gitignore files in the tree
  scanForGitignoreFiles(mRootPath);
}

bool GitignoreManager::isActive() const {
  return mActive;
}

bool GitignoreManager::isIgnored(const std::string &fullPath, bool isDirectory) const {
  if (!mActive) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mMutex);

  std::string normalizedPath = normalizePath(fullPath);
  std::string relativePath = getRelativePath(normalizedPath);

  if (relativePath.empty()) {
    return false;
  }

  // Last-match-wins semantics. We check from lowest priority to highest:
  // 1. Global gitignore (lowest priority)
  // 2. .git/info/exclude
  // 3. Root .gitignore and subdirectory .gitignore files (highest priority)

  bool ignored = false;

  // 1. Global gitignore rules (lowest priority)
  checkRules(mGlobalRules, relativePath, isDirectory, ignored);

  // 2. .git/info/exclude rules
  checkRules(mExcludeFileRules, relativePath, isDirectory, ignored);

  // 3. Walk from root directory down to the file's parent directory.
  //    Each .gitignore's rules are checked against the path relative to
  //    THAT .gitignore's directory. Closer .gitignore files have higher priority.
  std::string parentPath = normalizedPath;
  auto lastSlash = parentPath.rfind('/');
  if (lastSlash != std::string::npos) {
    parentPath = parentPath.substr(0, lastSlash);
  }

  // Build the list of directories from root to parent
  std::vector<std::string> dirsToCheck;
  std::string current = mRootPath;
  dirsToCheck.push_back(current);

  // Split the relative parent path into components and build intermediate paths
  std::string relParent = getRelativePath(parentPath);
  if (!relParent.empty()) {
    std::istringstream pathStream(relParent);
    std::string component;
    while (std::getline(pathStream, component, '/')) {
      if (!component.empty()) {
        current += "/" + component;
        dirsToCheck.push_back(current);
      }
    }
  }

  // Apply rules from each directory (root first, closest parent last)
  for (const auto &dir : dirsToCheck) {
    auto it = mRulesByDirectory.find(dir);
    if (it != mRulesByDirectory.end()) {
      std::string relFromDir = getRelativePathFrom(normalizedPath, dir);
      checkRules(it->second, relFromDir, isDirectory, ignored);
    }
  }

  return ignored;
}

void GitignoreManager::checkRules(
  const std::vector<GitignoreRule> &rules,
  const std::string &relativePath,
  bool isDirectory,
  bool &ignored
) const {
  // Extract basename for unanchored pattern matching
  std::string basename = relativePath;
  auto lastSlash = relativePath.rfind('/');
  if (lastSlash != std::string::npos) {
    basename = relativePath.substr(lastSlash + 1);
  }

  for (const auto &rule : rules) {
    // Directory-only rules only apply to directories
    if (rule.isDirectoryOnly && !isDirectory) {
      continue;
    }

    bool matches = false;

    if (rule.isAnchored) {
      // Anchored patterns: match against the full relative path
      matches = GitignoreParser::matchPattern(rule.pattern, relativePath, mCaseSensitive);
    } else {
      // Unanchored patterns: match against basename OR full relative path
      matches = GitignoreParser::matchPattern(rule.pattern, basename, mCaseSensitive)
             || GitignoreParser::matchPattern(rule.pattern, relativePath, mCaseSensitive);
    }

    if (matches) {
      // Last match wins: negation un-ignores, normal rule ignores
      ignored = !rule.isNegation;
    }
  }
}

void GitignoreManager::scanForGitignoreFiles(const std::string &dirPath) {
  // Check for .gitignore in this directory
  std::string gitignorePath = dirPath + "/.gitignore";
  if (fileExists(gitignorePath)) {
    loadGitignoreFile(dirPath);
  }

  // Recurse into subdirectories
#if defined(_WIN32)
  std::string searchPath = dirPath + "/*";
  WIN32_FIND_DATAA findData;
  HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
  if (hFind == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    std::string name(findData.cFileName);
    if (name == "." || name == ".." || name == ".git") {
      continue;
    }
    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      scanForGitignoreFiles(dirPath + "/" + name);
    }
  } while (FindNextFileA(hFind, &findData));
  FindClose(hFind);
#else
  DIR *dir = opendir(dirPath.c_str());
  if (dir == nullptr) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name(entry->d_name);
    if (name == "." || name == ".." || name == ".git") {
      continue;
    }
    std::string fullEntryPath = dirPath + "/" + name;
    struct stat st;
    if (stat(fullEntryPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      scanForGitignoreFiles(fullEntryPath);
    }
  }
  closedir(dir);
#endif
}

void GitignoreManager::loadGitignoreFile(const std::string &dirPath) {
  std::string gitignorePath = dirPath + "/.gitignore";
  std::string content = readFileContents(gitignorePath);
  if (content.empty()) {
    return;
  }
  std::string normalizedDir = normalizePath(dirPath);
  mRulesByDirectory[normalizedDir] = GitignoreParser::parse(content);
}

void GitignoreManager::loadGlobalGitignore(const std::string &customPath) {
  std::string path;

  if (!customPath.empty()) {
    // User-provided path via API
    path = customPath;
  } else {
    // Try platform-specific default locations
    std::string home = getHomeDirectory();
    if (!home.empty()) {
#if defined(_WIN32)
      // Windows: C:\Users\<user>\.gitignore_global
      std::string candidate = home + "/.gitignore_global";
      if (fileExists(candidate)) {
        path = candidate;
      }
#else
      // Linux/macOS: try ~/.gitignore_global first, then ~/.config/git/ignore
      std::string candidate1 = home + "/.gitignore_global";
      std::string candidate2 = home + "/.config/git/ignore";
      if (fileExists(candidate1)) {
        path = candidate1;
      } else if (fileExists(candidate2)) {
        path = candidate2;
      }
#endif
    }
  }

  if (path.empty() || !fileExists(path)) {
    return;
  }

  std::string content = readFileContents(path);
  if (!content.empty()) {
    mGlobalRules = GitignoreParser::parse(content);
  }
}

void GitignoreManager::loadGitInfoExclude() {
  std::string excludePath = mRootPath + "/.git/info/exclude";
  std::string content = readFileContents(excludePath);
  if (!content.empty()) {
    mExcludeFileRules = GitignoreParser::parse(content);
  }
}

std::string GitignoreManager::getRelativePath(const std::string &fullPath) const {
  return getRelativePathFrom(fullPath, mRootPath);
}

std::string GitignoreManager::getRelativePathFrom(
  const std::string &fullPath,
  const std::string &basePath
) const {
  // Both paths should already be normalized (no trailing slash)
  if (fullPath.size() <= basePath.size()) {
    return "";
  }
  if (fullPath.compare(0, basePath.size(), basePath) != 0) {
    return "";
  }
  // The character after basePath should be '/'
  if (fullPath[basePath.size()] != '/') {
    return "";
  }
  return fullPath.substr(basePath.size() + 1);
}

std::string GitignoreManager::getHomeDirectory() const {
#if defined(_WIN32)
  // Windows: use USERPROFILE or SHGetFolderPath
  const char *userProfile = getenv("USERPROFILE");
  if (userProfile != nullptr) {
    return normalizePath(std::string(userProfile));
  }
  char homePath[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, homePath))) {
    return normalizePath(std::string(homePath));
  }
  return "";
#else
  // Linux/macOS: use HOME or getpwuid
  const char *home = getenv("HOME");
  if (home != nullptr) {
    return std::string(home);
  }
  struct passwd *pw = getpwuid(getuid());
  if (pw != nullptr) {
    return std::string(pw->pw_dir);
  }
  return "";
#endif
}

void GitignoreManager::addOrUpdateGitignore(const std::string &directoryPath) {
  std::lock_guard<std::mutex> lock(mMutex);
  std::string normalizedDir = normalizePath(directoryPath);
  std::string gitignorePath = normalizedDir + "/.gitignore";
  if (fileExists(gitignorePath)) {
    std::string content = readFileContents(gitignorePath);
    if (!content.empty()) {
      mRulesByDirectory[normalizedDir] = GitignoreParser::parse(content);
    } else {
      mRulesByDirectory.erase(normalizedDir);
    }
  } else {
    mRulesByDirectory.erase(normalizedDir);
  }
}

void GitignoreManager::removeGitignore(const std::string &directoryPath) {
  std::lock_guard<std::mutex> lock(mMutex);
  std::string normalizedDir = normalizePath(directoryPath);
  mRulesByDirectory.erase(normalizedDir);
}
