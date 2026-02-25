#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(const std::string &path, const std::vector<std::string> &excludedPaths, std::shared_ptr<EventQueue> queue,
  bool respectGitignore, const std::string &globalGitignorePath) {
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, respectGitignore, globalGitignorePath));
}

NativeInterface::~NativeInterface() {
  mNativeInterface.reset(nullptr);
}

std::string NativeInterface::getError() {
  return mNativeInterface->getError();
}

bool NativeInterface::hasErrored() {
  return mNativeInterface->hasErrored();
}

bool NativeInterface::isWatching() {
  return mNativeInterface->isWatching();
}

void NativeInterface::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  mNativeInterface->updateExcludedPaths(excludedPaths);
}
