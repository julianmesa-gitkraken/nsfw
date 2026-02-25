#ifndef NSFW_FS_EVENTS_SERVICE_H
#define NSFW_FS_EVENTS_SERVICE_H

#include "RunLoop.h"
#include "../Queue.h"
#include "../../includes/GitignoreManager.h"

#include <CoreServices/CoreServices.h>
#include <time.h>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <vector>
#include <string>

void FSEventsServiceCallback(
  ConstFSEventStreamRef streamRef,
  void *clientCallBackInfo,
  size_t numEvents,
  void *eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
);

class RunLoop;

class FSEventsService {
public:
  FSEventsService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
    bool respectGitignore = false, const std::string &globalGitignorePath = "");

  friend void FSEventsServiceCallback(
    ConstFSEventStreamRef streamRef,
    void *clientCallBackInfo,
    size_t numEvents,
    void *eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]
  );
  std::string getError();
  bool hasErrored();
  bool isWatching();
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~FSEventsService();
private:
  void create(std::string path);
  void demangle(std::string path);
  void dispatch(EventType action, std::string path);
  void modify(std::string path);
  void remove(std::string path);
  void rename(std::vector<std::string> &paths);
  void splitFilePath(std::string &directory, std::string &name, const std::string &path);
  bool isExcluded(std::string filePath, bool isDirectory);
  bool isGitignoreFile(const std::string &path);

  std::string mPath;
  RunLoop *mRunLoop;
  std::shared_ptr<EventQueue> mQueue;
  std::vector<CFStringRef> mExcludedPaths;
  bool mRootChanged;
  bool mCaseSensitive;
  std::unique_ptr<GitignoreManager> mGitignoreManager;
};

#endif
