# Implementation Plan: `.gitignore` Pattern Matching Support for NSFW

## Design Decisions

The following decisions were confirmed during the design review:

| # | Question | Decision |
|---|----------|----------|
| 1 | Dynamic `.gitignore` reaction | **YES** — the watcher dynamically reacts when `.gitignore` files are created, modified, or deleted while watching is active |
| 2 | Global gitignore location | **Configurable via API** (`globalGitignorePath` option), with platform-specific fallbacks: Linux/macOS: `~/.gitignore_global`, `~/.config/git/ignore`; Windows: `C:\Users\<user>\.gitignore_global` |
| 3 | `.git/info/exclude` | **YES** — respect this file |
| 4 | Non-git repositories | **Only activate if `.git` directory exists** in the watched path. Add a comment in the code to make it easy to change this behavior later |
| 5 | Interaction with `excludedPaths` | **Additive** — both apply; a path is excluded if it matches either mechanism |
| 6 | `.git` directory exclusion | **NO** — do NOT automatically exclude the `.git` directory |
| 7 | Pattern matching engine | **Use git's `wildmatch.c`** (BSD license, battle-tested) |
| 8 | Runtime toggle | **NO** — `respectGitignore` is immutable, set only at watcher creation time |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    JavaScript Layer                          │
│  js/src/index.js  +  index.d.ts                             │
│  - New option: respectGitignore: boolean                    │
│  - New option: globalGitignorePath?: string                 │
│  - Passed through to C++ NSFW constructor                   │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│                    C++ Binding Layer                         │
│  NSFW.cpp / NSFW.h                                          │
│  - Parse respectGitignore + globalGitignorePath from options│
│  - Store mRespectGitignore, mGlobalGitignorePath            │
│  - Pass to NativeInterface                                  │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│                  NativeInterface                             │
│  NativeInterface.h / NativeInterface.cpp                    │
│  - New constructor params: bool respectGitignore,           │
│    std::string globalGitignorePath                          │
│  - Pass to platform implementation                          │
└──────────────────────┬──────────────────────────────────────┘
                       │
        ┌──────────────┼──────────────┐
        │              │              │
┌───────▼──────┐ ┌─────▼──────┐ ┌────▼───────┐
│    Linux     │ │   macOS    │ │  Windows   │
│ InotifyServ. │ │ FSEventsSv.│ │ Controller │
│ InotifyTree  │ │            │ │  Watcher   │
│              │ │            │ │            │
│ Uses Gitign. │ │ Uses Gitign│ │Uses Gitign.│
│ Manager to:  │ │ Manager to:│ │Manager to: │
│ 1.Skip watch │ │ 1.Filter   │ │1.Filter    │
│   creation   │ │   events   │ │  events    │
│ 2.Filter     │ │            │ │            │
│   events     │ │            │ │            │
└───────┬──────┘ └─────┬──────┘ └────┬───────┘
        │              │              │
        └──────────────┼──────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│              NEW: GitignoreManager (cross-platform)          │
│  includes/GitignoreManager.h  /  src/GitignoreManager.cpp   │
│  - Checks for .git directory presence (Decision #4)         │
│  - Scans directory tree for .gitignore files                │
│  - Loads .git/info/exclude (Decision #3)                    │
│  - Loads global gitignore with fallbacks (Decision #2)      │
│  - Builds hierarchical rule set                             │
│  - Provides: bool isIgnored(path, isDirectory) const        │
│  - Provides: dynamic update methods (Decision #1)           │
│  - Thread-safe with mutex                                   │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│              NEW: GitignoreParser (cross-platform)           │
│  includes/GitignoreParser.h  /  src/GitignoreParser.cpp     │
│  - Parses a single .gitignore file into rules               │
│  - Wraps git's wildmatch.c for pattern matching (Dec. #7)   │
│  - Supports: *, **, ?, [charset], negation (!), dir-only (/)│
└─────────────────────────────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│              VENDORED: wildmatch.c / wildmatch.h            │
│  src/wildmatch.c  /  includes/wildmatch.h                   │
│  - Git's own pattern matching (BSD license)                 │
│  - Compiled as C, linked with C++ code                      │
└─────────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Implementation Plan

### Phase 1: Core Gitignore Engine (Cross-Platform)

#### Step 1.0: Vendor `wildmatch.c` from Git

**New files:**
- `includes/wildmatch.h`
- `src/wildmatch.c`

**Actions:**
- Copy `wildmatch.c` and `wildmatch.h` from the git source tree (BSD-licensed)
- Minimal modifications to make it compile standalone (remove git-specific includes)
- The `wildmatch()` function signature: `int wildmatch(const char *pattern, const char *text, unsigned int flags)`
- Key flags: `WM_PATHNAME` (don't match `/` with `*`), `WM_CASEFOLD` (case-insensitive)

#### Step 1.1: Create `GitignoreParser` — Pattern Parsing + Matching

**New files:**
- `includes/GitignoreParser.h`
- `src/GitignoreParser.cpp`

**Responsibilities:**
- Parse a single `.gitignore` file content into a list of `GitignoreRule` structs
- Each rule contains: the cleaned pattern, negation flag, directory-only flag, anchored flag
- Delegate actual pattern matching to `wildmatch()`
- Handle `.gitignore` syntax:
  - Leading `#` → comment (skip)
  - Leading `\#` → escaped hash (literal `#`)
  - Trailing spaces → stripped unless escaped with `\`
  - Empty lines → skip
  - `!pattern` → negation (re-include)
  - `pattern/` → directory-only match
  - Patterns containing `/` (not just trailing) → anchored to `.gitignore` location
  - Patterns without `/` → match at any depth
  - `**` in patterns → match across directory boundaries

**Key data structures:**
```cpp
struct GitignoreRule {
    std::string pattern;       // The cleaned pattern (without leading ! or trailing /)
    bool isNegation;           // Started with !
    bool isDirectoryOnly;      // Ended with /
    bool isAnchored;           // Contains / (not just trailing)
};

class GitignoreParser {
public:
    // Parse .gitignore file content into rules
    static std::vector<GitignoreRule> parse(const std::string &content);

    // Check if a relative path matches a single rule's pattern
    // Uses wildmatch() internally
    static bool matchPattern(
        const std::string &pattern,
        const std::string &relativePath,
        bool caseSensitive = true
    );
};
```

#### Step 1.2: Create `GitignoreManager` — Hierarchical Rule Manager

**New files:**
- `includes/GitignoreManager.h`
- `src/GitignoreManager.cpp`

**Responsibilities:**
- On construction, check for `.git` directory presence (**Decision #4**). If absent, `isIgnored()` always returns `false`. Add a code comment: `// NOTE: To enable gitignore support without a .git directory, remove this check.`
- Scan the watched directory tree for `.gitignore` files
- Load `.git/info/exclude` rules (**Decision #3**) — scoped to the entire repo
- Load global gitignore (**Decision #2**):
  - If `globalGitignorePath` is provided via API, use that
  - Otherwise, try platform-specific fallback locations in order:
    - Linux/macOS: `~/.gitignore_global`, then `~/.config/git/ignore`
    - Windows: `C:\Users\<user>\.gitignore_global`
  - If none found, silently continue (no error)
- Build a hierarchical map: `directory_path → vector<GitignoreRule>`
- Provide `bool isIgnored(const std::string &fullPath, bool isDirectory) const`
- Provide dynamic update methods (**Decision #1**):
  - `void addOrUpdateGitignore(const std::string &directoryPath)` — re-reads `.gitignore` in that directory
  - `void removeGitignore(const std::string &directoryPath)` — removes rules for that directory
- Thread-safe via `std::mutex`

**Key data structures:**
```cpp
class GitignoreManager {
public:
    GitignoreManager(
        const std::string &rootPath,
        const std::string &globalGitignorePath = "",
        bool caseSensitive = true
    );

    // Returns true if the path should be ignored
    // Checks excludeFile, global gitignore, and hierarchical .gitignore rules
    bool isIgnored(const std::string &fullPath, bool isDirectory) const;

    // Returns true if gitignore support is active (.git directory was found)
    bool isActive() const;

    // Dynamic updates — called when .gitignore files change at runtime
    void addOrUpdateGitignore(const std::string &directoryPath);
    void removeGitignore(const std::string &directoryPath);

private:
    void scanForGitignoreFiles(const std::string &rootPath);
    void loadGitignoreFile(const std::string &dirPath);
    void loadGlobalGitignore(const std::string &customPath);
    void loadGitInfoExclude();
    std::string getRelativePath(const std::string &fullPath) const;
    std::string getHomeDirectory() const;

    std::string mRootPath;
    bool mActive;                    // false if no .git directory found
    bool mCaseSensitive;
    std::vector<GitignoreRule> mGlobalRules;       // From global gitignore
    std::vector<GitignoreRule> mExcludeFileRules;  // From .git/info/exclude
    // Map from directory path to its .gitignore rules
    std::map<std::string, std::vector<GitignoreRule>> mRulesByDirectory;
    mutable std::mutex mMutex;
};
```

**Matching algorithm for `isIgnored()`:**
```
1. If !mActive, return false
2. Start with result = NOT_MATCHED
3. Apply global gitignore rules against the relative path
4. Apply .git/info/exclude rules against the relative path
5. Walk from root directory down to the file's parent directory:
   a. For each directory that has .gitignore rules:
      - Compute the path relative to THAT directory
      - Check each rule in order
      - If rule is directory-only and path is not a directory: skip
      - If rule matches:
        - If negation: result = NOT_IGNORED
        - Else: result = IGNORED
6. Return result == IGNORED
```

**Rule priority (last match wins, within each level):**
```
Global gitignore (lowest priority)
  └─ .git/info/exclude
       └─ Root .gitignore
            └─ Subdirectory .gitignore (highest priority)
```

---

### Phase 2: Platform Integration

#### Step 2.1: Linux — `InotifyTree` and `InotifyService`

**Files to modify:**
- `includes/linux/InotifyTree.h`
- `src/linux/InotifyTree.cpp`
- `includes/linux/InotifyService.h`
- `src/linux/InotifyService.cpp`

**Changes to `InotifyTree`:**

1. Add `std::shared_ptr<GitignoreManager>` member
2. Constructor: accept `bool respectGitignore` and `const std::string &globalGitignorePath` parameters. If `respectGitignore` is true, create a `GitignoreManager` for the watch root
3. Modify `addDirectoryTree()` (the key Linux optimization):
   - During the `recursive_directory_iterator` loop, before the existing `isExcludedPath()` check, also check `mGitignoreManager->isIgnored(entryPath, isDirectory)`
   - If ignored AND is a directory: call `iterator.disable_recursion_pending()` — **skips the entire subtree, saving inotify watch descriptors and memory**
   - If ignored AND is a file: `continue` (skip the file — no watch needed since files don't get individual watches, but this prevents scanning)
   - Also during the scan: if a `.gitignore` file is found in a directory, call `mGitignoreManager->addOrUpdateGitignore(dirPath)` so rules are available for subsequent entries
4. Modify `addDirectory()` (called when new directories are created at runtime):
   - Check `mGitignoreManager->isIgnored(childPath, true)` before creating the watch
   - After adding the watch, check if the new directory contains a `.gitignore` file and call `mGitignoreManager->addOrUpdateGitignore()` if so
5. Expose `getGitignoreManager()` so `InotifyService` can access it

**Changes to `InotifyService`:**

1. Accept `bool respectGitignore` and `const std::string &globalGitignorePath` in constructor, pass to `InotifyTree`
2. Access `GitignoreManager` via `mTree->getGitignoreManager()`
3. Modify `dispatch()`: Before enqueuing, check `mGitignoreManager->isIgnored(fullPath, false)` — **event-level filtering as a safety net** (additive with `excludedPaths`, per Decision #5)
4. Modify `createDirectory()`: When a new directory is created, after adding it to the tree, check if it contains a `.gitignore` file
5. **`.gitignore` file change detection** (Decision #1 — dynamic reaction):
   - In `create()` and `modify()`: if the file name is `.gitignore`, call `mGitignoreManager->addOrUpdateGitignore(directoryPath)`
     - For newly ignored directories: remove their inotify watches via `mTree->removeDirectory()`
     - This is complex — we need to re-evaluate all watched paths under the affected directory
   - In `remove()`: if the file name is `.gitignore`, call `mGitignoreManager->removeGitignore(directoryPath)`
     - For previously ignored directories that are now un-ignored: add watches via `mTree->addDirectory()`
   - In `rename()`: handle `.gitignore` being renamed to/from

**Linux-specific optimization detail:**
The current `excludedPaths` on Linux works by NOT creating inotify watches (proactive exclusion in `addDirectoryTree`) AND by not having watches to fire events (no reactive filtering needed for excluded subtrees). The gitignore feature follows the same dual approach:
- **Proactive**: Don't create watches for ignored paths → saves inotify watch descriptors (limited system resource)
- **Reactive**: Filter events in `dispatch()` as a safety net for edge cases (e.g., a file created between the gitignore check and the watch creation)

#### Step 2.2: macOS — `FSEventsService`

**Files to modify:**
- `includes/osx/FSEventsService.h`
- `src/osx/FSEventsService.cpp`

**Changes:**

1. Add `std::unique_ptr<GitignoreManager>` member
2. Constructor: accept `bool respectGitignore` and `const std::string &globalGitignorePath`. If `respectGitignore` is true, create `GitignoreManager`
3. Modify `isExcluded()` to also check gitignore:
   - After the existing `excludedPaths` prefix check (Decision #5 — additive), also check `mGitignoreManager->isIgnored(filePath, isDirectory)`
   - Determine `isDirectory` using FSEvents flags (`kFSEventStreamEventFlagItemIsDir`) — pass this info from the callback
4. In `FSEventsServiceCallback()`: the `isExcluded()` call already gates all event processing. Extend it to also check gitignore rules
5. **`.gitignore` file change detection** (Decision #1):
   - When events for files named `.gitignore` are detected (create/modify/delete), update the `GitignoreManager`
   - No watch tree to modify on macOS — only the filtering rules change

**Note:** macOS FSEvents doesn't have a watch tree to optimize (it watches the entire directory recursively via the kernel). The benefit is purely event filtering — preventing unnecessary events from reaching JavaScript.

#### Step 2.3: Windows — `Controller` and `Watcher`

**Files to modify:**
- `includes/win32/Controller.h`
- `src/win32/Controller.cpp`
- `includes/win32/Watcher.h`
- `src/win32/Watcher.cpp`

**Changes to `Controller`:**

1. Accept `bool respectGitignore` and `const std::string &globalGitignorePath` in constructor
2. Pass to `Watcher` (using UTF-8 path, not wide path — gitignore matching is done in UTF-8)

**Changes to `Watcher`:**

1. Add `std::unique_ptr<GitignoreManager>` member
2. Constructor: if `respectGitignore`, create `GitignoreManager` using the UTF-8 version of the watch path
3. Modify `isExcluded()`: After the existing wide-string prefix check (Decision #5 — additive), also check `mGitignoreManager->isIgnored(utf8FullPath, isDirectory)`
   - Convert the wide `fileName` to UTF-8 for gitignore matching (reuse existing `getUTF8FileName`/`getUTF8Directory` helpers)
   - Determine if the path is a directory — can check via `GetFileAttributesW` or track from the event action type
4. In `handleEvents()`: the `isExcluded()` call already gates event processing
5. **`.gitignore` file change detection** (Decision #1):
   - When events for files named `.gitignore` are detected, update the `GitignoreManager`
   - No watch tree to modify on Windows — only the filtering rules change

**Windows-specific considerations:**
- Case sensitivity: Windows is case-insensitive. Pass `caseSensitive = false` to `GitignoreManager`
- Path separators: Normalize `\` to `/` before gitignore matching
- Global gitignore fallback: `C:\Users\<user>\.gitignore_global` — use `USERPROFILE` environment variable

---

### Phase 3: Binding Layer Changes

#### Step 3.1: `NativeInterface`

**Files to modify:**
- `includes/NativeInterface.h`
- `src/NativeInterface.cpp`

**Changes:**
1. Add `bool respectGitignore` and `std::string globalGitignorePath` parameters to constructor
2. Pass to `NativeImplementation` constructor (which is `InotifyService`, `FSEventsService`, or `Controller` depending on platform)

#### Step 3.2: `NSFW` (N-API Binding)

**Files to modify:**
- `includes/NSFW.h`
- `src/NSFW.cpp`

**Changes:**
1. Add members: `bool mRespectGitignore` and `std::string mGlobalGitignorePath`
2. In constructor: parse `options.respectGitignore` (boolean, default `false`) — **immutable after creation (Decision #8)**
3. In constructor: parse `options.globalGitignorePath` (string, optional)
4. In `StartWorker::Execute()`: pass `mRespectGitignore` and `mGlobalGitignorePath` to `NativeInterface` constructor
5. No new `UpdateRespectGitignore` worker needed (Decision #8 — immutable)

---

### Phase 4: JavaScript Layer Changes

#### Step 4.1: `js/src/index.js`

**Changes:**
1. In `buildNSFW()` options destructuring: extract `respectGitignore = false` and `globalGitignorePath`
2. Pass both to `NSFW` constructor in the options object
3. Validation: `respectGitignore` must be boolean, `globalGitignorePath` must be string if provided

#### Step 4.2: `index.d.ts`

**Changes:**
```typescript
interface Options {
    /** time in milliseconds to debounce the event callback */
    debounceMS?: number;
    /** callback to fire in the case of errors */
    errorCallback?: (err: any) => void;
    /** paths to be excluded */
    excludedPaths?: string[];
    /**
     * If true, respect .gitignore patterns when filtering events.
     * Only activates if a .git directory exists in the watched path.
     * Works additively with excludedPaths.
     * Cannot be changed after the watcher is created.
     */
    respectGitignore?: boolean;
    /**
     * Custom path to the global gitignore file.
     * If not provided, falls back to platform defaults:
     * - Linux/macOS: ~/.gitignore_global, ~/.config/git/ignore
     * - Windows: C:\Users\<user>\.gitignore_global
     */
    globalGitignorePath?: string;
}
```

---

### Phase 5: Build System Changes

#### Step 5.1: `binding.gyp`

**Changes:**
1. Add `src/GitignoreParser.cpp`, `src/GitignoreManager.cpp`, and `src/wildmatch.c` to the platform-independent `sources` array
2. Note: `wildmatch.c` is C code — ensure the build system compiles it as C (node-gyp handles `.c` files automatically)

#### Step 5.2: `src/CMakeLists.txt`

**Changes:**
1. Add `GitignoreParser.cpp`, `GitignoreManager.cpp`, and `wildmatch.c` to `NSFW_LIBRARY_SOURCES`

---

### Phase 6: Testing

#### Step 6.1: Test Plan for `js/spec/index-spec.js`

Add a new `describe('Gitignore support', ...)` block with tests:

1. **Basic gitignore filtering**: Create a `.gitignore` with `*.log`, create `test.log` and `test.txt`, verify only `test.txt` events are received
2. **Directory pattern**: `.gitignore` with `build/`, create files inside `build/`, verify no events
3. **Negation**: `.gitignore` with `*.log` and `!important.log`, verify `important.log` events ARE received
4. **Hierarchical override**: Root `.gitignore` ignores `*.tmp`, subdirectory `.gitignore` negates `!keep.tmp`, verify `keep.tmp` events in subdirectory ARE received
5. **Wildcard patterns**: Test `**/*.o`, `doc/**`, `**/foo`
6. **Character classes**: Test `[Dd]ebug/`
7. **Anchored patterns**: Test `/TODO` (only root) vs `TODO` (any depth)
8. **Comment and blank lines**: Verify `#comment` and blank lines are ignored
9. **Interaction with excludedPaths** (Decision #5): Verify both mechanisms work together — a path excluded by either is filtered
10. **No `.gitignore` file**: Verify `respectGitignore: true` works fine when no `.gitignore` exists (no errors, no filtering)
11. **No `.git` directory** (Decision #4): Verify `respectGitignore: true` does nothing when no `.git` directory exists
12. **Dynamic `.gitignore` creation** (Decision #1): Create `.gitignore` after watcher starts, verify filtering begins
13. **Dynamic `.gitignore` modification** (Decision #1): Modify `.gitignore` while watching, verify new rules apply
14. **Dynamic `.gitignore` deletion** (Decision #1): Delete `.gitignore` while watching, verify filtering stops
15. **`.git/info/exclude`** (Decision #3): Create `.git/info/exclude` with patterns, verify they are respected
16. **Global gitignore** (Decision #2): Test with `globalGitignorePath` option
17. **`.git` directory NOT excluded** (Decision #6): Verify events from `.git/` directory ARE received
18. **Linux-specific** (if on Linux): Verify that ignored directories don't consume inotify watches (create many ignored directories, verify no `ENOSPC` error)
19. **respectGitignore immutability** (Decision #8): Verify the option cannot be changed after creation (no API method exists)

---

## File Summary

### New Files (6)
| File | Purpose |
|------|---------|
| `includes/wildmatch.h` | Git's wildmatch header (vendored, BSD license) |
| `src/wildmatch.c` | Git's wildmatch implementation (vendored, BSD license) |
| `includes/GitignoreParser.h` | Pattern parsing and matching engine header |
| `src/GitignoreParser.cpp` | Pattern parsing and matching engine implementation |
| `includes/GitignoreManager.h` | Hierarchical rule manager header |
| `src/GitignoreManager.cpp` | Hierarchical rule manager implementation |

### Modified Files (16+)
| File | Changes |
|------|---------|
| `includes/NSFW.h` | Add `mRespectGitignore`, `mGlobalGitignorePath` members |
| `src/NSFW.cpp` | Parse `respectGitignore` and `globalGitignorePath` options, pass to NativeInterface |
| `includes/NativeInterface.h` | Add `respectGitignore`, `globalGitignorePath` constructor params |
| `src/NativeInterface.cpp` | Pass new params to platform implementation |
| `includes/linux/InotifyTree.h` | Add `GitignoreManager` member |
| `src/linux/InotifyTree.cpp` | Check gitignore in `addDirectoryTree()`, `addDirectory()` |
| `includes/linux/InotifyService.h` | Store reference to `GitignoreManager` |
| `src/linux/InotifyService.cpp` | Event-level filtering, `.gitignore` change detection |
| `includes/osx/FSEventsService.h` | Add `GitignoreManager` member |
| `src/osx/FSEventsService.cpp` | Extend `isExcluded()` with gitignore check, `.gitignore` change detection |
| `includes/win32/Controller.h` | Add `respectGitignore`, `globalGitignorePath` params |
| `src/win32/Controller.cpp` | Pass new params to Watcher |
| `includes/win32/Watcher.h` | Add `GitignoreManager` member |
| `src/win32/Watcher.cpp` | Extend `isExcluded()` with gitignore check, `.gitignore` change detection |
| `js/src/index.js` | Extract and pass `respectGitignore` and `globalGitignorePath` options |
| `index.d.ts` | Add `respectGitignore` and `globalGitignorePath` to Options interface |
| `binding.gyp` | Add new source files |
| `src/CMakeLists.txt` | Add new source files |
| `js/spec/index-spec.js` | Add gitignore test suite |

---

## Edge Cases and Considerations

1. **Symlinks**: `.gitignore` patterns should NOT follow symlinks (consistent with git behavior). The existing codebase already handles symlink loops (test exists in `index-spec.js`).

2. **Case sensitivity**:
   - Linux: case-sensitive matching (default)
   - macOS: case-insensitive matching (the existing code already detects this via `pathconf(..., _PC_CASE_SENSITIVE)`)
   - Windows: case-insensitive matching
   - The `GitignoreManager` accepts a `caseSensitive` flag, set per-platform

3. **Path separator normalization**:
   - `.gitignore` uses `/` as separator
   - Windows uses `\`
   - The `GitignoreManager` normalizes all paths to `/` before matching

4. **Race conditions**:
   - On Linux, a `.gitignore` file could be created in a new directory before we scan it. The `addDirectory()` method should check for `.gitignore` after adding the watch
   - Thread safety: `GitignoreManager` uses a mutex since it's accessed from the event loop thread and updated from the same thread

5. **Large repositories**: Scanning for `.gitignore` files during startup adds latency. Mitigations:
   - On Linux: scan for `.gitignore` files during the same `recursive_directory_iterator` pass in `addDirectoryTree()` — no extra traversal
   - On macOS/Windows: need a separate initial scan since they don't walk the tree at startup

6. **Binary `.gitignore` files**: Handle gracefully — skip lines with null bytes

7. **UTF-8 patterns**: `.gitignore` files are UTF-8. `wildmatch()` operates on bytes, which works correctly for UTF-8 with `*` and `?` matching individual bytes (multi-byte sequences are preserved)

8. **Empty watch root**: If the watch root itself matches a gitignore pattern, it should NOT be ignored (we're explicitly watching it)

9. **`.gitignore` in excluded paths**: If a directory is in `excludedPaths`, we should NOT scan it for `.gitignore` files (it's already excluded)

10. **Performance**: The `isIgnored()` check is called for every event. Complexity is O(d × r) where d = directory depth and r = average rules per directory. For most repositories this is very fast. Consider caching results if profiling shows issues.

11. **Global gitignore file not found**: Silently continue — many systems don't have a global gitignore file

12. **Nested `.git` directories (submodules)**: Each `.gitignore` works hierarchically from the watch root, not from each `.git` directory. This is a simplification vs full git behavior.

13. **`.gitignore` events themselves**: Events for `.gitignore` file changes should still be dispatched to JavaScript (the file itself is not ignored). Only the *rules* are updated internally.

14. **Concurrent `.gitignore` updates**: If multiple `.gitignore` files change simultaneously (e.g., during a `git checkout`), each update is processed independently. The mutex ensures consistency.

15. **Maximum pattern count**: No artificial limit on the number of patterns. However, extremely large `.gitignore` files (thousands of patterns) could impact performance.

---

## Recommended Implementation Order

1. **Phase 1** — Build and unit-test `wildmatch.c`, `GitignoreParser`, and `GitignoreManager` in isolation
2. **Phase 2.1** (Linux) — Linux platform integration (primary optimization target)
3. **Phase 3** — Binding layer changes (`NativeInterface`, `NSFW`)
4. **Phase 4** — JavaScript layer (`index.js`, `index.d.ts`)
5. **Phase 5** — Build system (`binding.gyp`, `CMakeLists.txt`)
6. **Phase 6** — Integration tests
7. **Phase 2.2** (macOS) and **Phase 2.3** (Windows) — Can be done in parallel or later