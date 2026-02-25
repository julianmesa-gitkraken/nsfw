#include "../../includes/linux/InotifyService.h"

InotifyService::InotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
  bool respectGitignore, const std::string &globalGitignorePath):
  mEventLoop(NULL),
  mQueue(queue),
  mTree(NULL) {
  mInotifyInstance = inotify_init();

  if (mInotifyInstance == -1) {
    return;
  }

  mTree = new InotifyTree(mInotifyInstance, path, excludedPaths, respectGitignore, globalGitignorePath);
  if (!mTree->isRootAlive()) {
    delete mTree;
    mTree = NULL;
    mEventLoop = NULL;
    close(mInotifyInstance);
  } else {
    mEventLoop = new InotifyEventLoop(
      mInotifyInstance,
      this
    );
  }
}

InotifyService::~InotifyService() {
  if (mEventLoop != NULL) {
    delete mEventLoop;
  }

  if (mTree != NULL) {
    delete mTree;
  }

  close(mInotifyInstance);
}

void InotifyService::create(int wd, std::string name) {
  // Detect .gitignore file creation (Decision #1: dynamic reaction)
  if (name == ".gitignore") {
    auto gitignoreManager = mTree->getGitignoreManager();
    if (gitignoreManager && gitignoreManager->isActive()) {
      std::string dirPath;
      if (mTree->getPath(dirPath, wd)) {
        gitignoreManager->addOrUpdateGitignore(dirPath);
      }
    }
  }
  dispatch(CREATED, wd, name);
}

void InotifyService::dispatch(EventType action, int wd, std::string name) {
  std::string path;
  if (!mTree->getPath(path, wd)) {
    return;
  }

  // Gitignore event filtering as safety net (additive with excludedPaths, Decision #5)
  auto gitignoreManager = mTree->getGitignoreManager();
  if (gitignoreManager && gitignoreManager->isActive()) {
    std::string fullPath = path + "/" + name;
    // We don't know if it's a directory here, so check as file (false).
    // Directory events are handled separately via createDirectory/removeDirectory.
    if (gitignoreManager->isIgnored(fullPath, false)) {
      return;
    }
  }

  mQueue->enqueue(action, path, name);
}

void InotifyService::dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName) {
  std::string fromPath, toPath;
  if (!mTree->getPath(fromPath, fromWd) || !mTree->getPath(toPath, toWd)) {
    return;
  }

  mQueue->enqueue(RENAMED, fromPath, fromName, toPath, toName);
}

std::string InotifyService::getError() {
  if (mTree != NULL && mTree->hasErrored()) {
    return mTree->getError();
  }

  if (!isWatching()) {
    return "Service shutdown unexpectedly";
  }

  return "";
}

bool InotifyService::hasErrored() {
  return !isWatching() || (mTree == NULL ? false : mTree->hasErrored());
}

bool InotifyService::isWatching() {
  if (mTree == NULL || mEventLoop == NULL) {
    return false;
  }

  return mTree->isRootAlive() && mEventLoop->isLooping();
}

void InotifyService::modify(int wd, std::string name) {
  // Detect .gitignore file modification (Decision #1: dynamic reaction)
  if (name == ".gitignore") {
    auto gitignoreManager = mTree->getGitignoreManager();
    if (gitignoreManager && gitignoreManager->isActive()) {
      std::string dirPath;
      if (mTree->getPath(dirPath, wd)) {
        gitignoreManager->addOrUpdateGitignore(dirPath);
      }
    }
  }
  dispatch(MODIFIED, wd, name);
}

void InotifyService::remove(int wd, std::string name) {
  // Detect .gitignore file deletion (Decision #1: dynamic reaction)
  if (name == ".gitignore") {
    auto gitignoreManager = mTree->getGitignoreManager();
    if (gitignoreManager && gitignoreManager->isActive()) {
      std::string dirPath;
      if (mTree->getPath(dirPath, wd)) {
        gitignoreManager->removeGitignore(dirPath);
      }
    }
  }
  dispatch(DELETED, wd, name);
}

void InotifyService::rename(int fromWd, std::string fromName, int toWd, std::string toName) {
  // Detect .gitignore being renamed to/from (Decision #1: dynamic reaction)
  auto gitignoreManager = mTree->getGitignoreManager();
  if (gitignoreManager && gitignoreManager->isActive()) {
    if (fromName == ".gitignore") {
      std::string fromDirPath;
      if (mTree->getPath(fromDirPath, fromWd)) {
        gitignoreManager->removeGitignore(fromDirPath);
      }
    }
    if (toName == ".gitignore") {
      std::string toDirPath;
      if (mTree->getPath(toDirPath, toWd)) {
        gitignoreManager->addOrUpdateGitignore(toDirPath);
      }
    }
  }
  dispatchRename(fromWd, fromName, toWd, toName);
}

void InotifyService::createDirectory(int wd, std::string name) {
  if (!mTree->nodeExists(wd)) {
    return;
  }

  dispatch(CREATED, wd, name);
  mTree->addDirectory(wd, name, [this](std::string directory, std::string file) {
    mQueue->enqueue(CREATED, directory, file);
  });

  // After adding the directory, check if it contains a .gitignore file
  auto gitignoreManager = mTree->getGitignoreManager();
  if (gitignoreManager && gitignoreManager->isActive()) {
    std::string dirPath;
    if (mTree->getPath(dirPath, wd)) {
      std::string newDirPath = dirPath + "/" + name;
      std::string gitignorePath = newDirPath + "/.gitignore";
      struct stat fileStat;
      if (stat(gitignorePath.c_str(), &fileStat) == 0 && S_ISREG(fileStat.st_mode)) {
        gitignoreManager->addOrUpdateGitignore(newDirPath);
      }
    }
  }
}

void InotifyService::removeDirectory(int wd) {
  mTree->removeDirectory(wd);
}

void InotifyService::renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName) {
  if (!mTree->nodeExists(fromWd) || !mTree->nodeExists(toWd)) {
    return;
  }

  mTree->renameDirectory(fromWd, fromName, toWd, toName);

  dispatchRename(fromWd, fromName, toWd, toName);
}

void InotifyService::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  mTree->updateExcludedPaths(excludedPaths);
}