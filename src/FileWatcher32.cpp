#include "../includes/FileWatcher32.h"

namespace NSFW {
  FSEventHandler::FSEventHandler(FileSystemWatcher^ parentFW, std::queue<Event> &eventsQueue, bool &watchFiles)
   : mParentFW(parentFW), mEventsQueue(eventsQueue), mWatchFiles(watchFiles) {}

  // Handles the generalized change event for changed/created/deleted and pushes event to queue
  void FSEventHandler::eventHandlerHelper(FileSystemEventArgs^ e, System::String^ action) {
    if (!mWatchFiles) {
      // Remove these handlers if the object is no longer listening (stop is called)
      removeHandlers();
      return;
    }
    Event event;
    event.directory = (char*)(void*)Marshal::StringToHGlobalAnsi(e->FullPath);
    event.file = new std::string((char*)(void*)Marshal::StringToHGlobalAnsi(e->Name));
    event.action = (char*)(void*)Marshal::StringToHGlobalAnsi(action);
    mEventsQueue.push(event);
  }

  void FSEventHandler::onChanged(Object^ source, FileSystemEventArgs^ e) {
    eventHandlerHelper(e, "CHANGED");
  }

  void FSEventHandler::onCreated(Object^ source, FileSystemEventArgs^ e) {
    eventHandlerHelper(e, "CREATED");
  }

  void FSEventHandler::onDeleted(Object^ source, FileSystemEventArgs^ e) {
    eventHandlerHelper(e, "DELETED");
  }

  // Specialized handler for renamed events, pushes to event queue
  void FSEventHandler::onRenamed(Object^ source, RenamedEventArgs^ e) {
    if (!mWatchFiles) {
      // Remove these handlers if the object is no longer listening (stop is called)
      removeHandlers();
      return;
    }
    Event event;
    event.directory = (char*)(void*)Marshal::StringToHGlobalAnsi(e->FullPath);
    event.file = new std::string[2];
    event.file[0] = (char*)(void*)Marshal::StringToHGlobalAnsi(e->OldName);
    event.file[1] = (char*)(void*)Marshal::StringToHGlobalAnsi(e->Name);
    event.action = "RENAMED";
    mEventsQueue.push(event);
  }

  void FSEventHandler::rememberHandlers(
    FileSystemEventHandler^ changed,
    FileSystemEventHandler^ created,
    FileSystemEventHandler^ deleted,
    RenamedEventHandler^ renamed
  ) {
    mChanged = changed;
    mCreated = created;
    mDeleted = deleted;
    mRenamed = renamed;
  }

  void FSEventHandler::removeHandlers() {
    mParentFW->Changed -= mChanged;
    mParentFW->Created -= mCreated;
    mParentFW->Deleted -= mDeleted;
    mParentFW->Renamed -= mRenamed;
  }

  // Creates the filewatcher and initializes the handlers.
  void createFileWatcher(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles) {
    FileSystemWatcher^ fsWatcher;
    FSEventHandler^ handler;

    fsWatcher = gcnew FileSystemWatcher();
    fsWatcher->Path = gcnew System::String(path.c_str());

    fsWatcher->NotifyFilter = static_cast<NotifyFilters>(
      NotifyFilters::FileName |
      NotifyFilters::Attributes |
      NotifyFilters::LastAccess |
      NotifyFilters::LastWrite |
      NotifyFilters::Security |
      NotifyFilters::Size
    );
    handler = gcnew FSEventHandler(fsWatcher, eventsQueue, watchFiles);

    // We want to remember the handler delegates we create, since we don't have a reference to this object in
    // unmanaged land we'll pass the handlers to the FSEventHandler to remove from itself when it posts an event
    // after mWatchFiles is set to false.
    FileSystemEventHandler^ changed = gcnew FileSystemEventHandler(
      handler, &FSEventHandler::onChanged);
    FileSystemEventHandler^ created = gcnew FileSystemEventHandler(
      handler, &FSEventHandler::onCreated);
    FileSystemEventHandler^ deleted = gcnew FileSystemEventHandler(
      handler, &FSEventHandler::onDeleted);
    RenamedEventHandler^ renamed = gcnew RenamedEventHandler(
      handler, &FSEventHandler::onRenamed);

    // pass the handler delegates to the handler
    handler->rememberHandlers(changed, created, deleted, renamed);

    fsWatcher->Changed += changed;
    fsWatcher->Created += created;
    fsWatcher->Deleted += deleted;
    fsWatcher->Renamed += renamed;

    fsWatcher->EnableRaisingEvents = true;
  }

}