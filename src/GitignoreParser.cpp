#include "../includes/GitignoreParser.h"
#include "../includes/wildmatch.h"

#include <sstream>

std::vector<GitignoreRule> GitignoreParser::parse(const std::string &content) {
  std::vector<GitignoreRule> rules;
  std::istringstream stream(content);
  std::string line;

  while (std::getline(stream, line)) {
    // Remove trailing \r if present (Windows line endings)
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Skip empty lines
    if (line.empty()) {
      continue;
    }

    // Skip comments (lines starting with #)
    // But \# is an escaped hash — a literal # pattern
    if (line[0] == '#') {
      continue;
    }

    // Handle escaped leading hash: \# → #
    if (line.size() >= 2 && line[0] == '\\' && line[1] == '#') {
      line = line.substr(1);
    }

    // Strip trailing whitespace, unless escaped with backslash.
    // A trailing "\ " means the space is significant.
    while (!line.empty() && line.back() == ' ') {
      if (line.size() >= 2 && line[line.size() - 2] == '\\') {
        // Escaped trailing space: remove the backslash, keep the space
        line.erase(line.size() - 2, 1);
        break;
      }
      line.pop_back();
    }

    // After stripping, skip if empty
    if (line.empty()) {
      continue;
    }

    GitignoreRule rule;
    rule.isNegation = false;
    rule.isDirectoryOnly = false;
    rule.isAnchored = false;

    // Handle negation prefix: !pattern
    if (line[0] == '!') {
      rule.isNegation = true;
      line = line.substr(1);
      if (line.empty()) {
        continue;
      }
    }
    // Handle escaped negation: \! → literal !
    else if (line.size() >= 2 && line[0] == '\\' && line[1] == '!') {
      line = line.substr(1);
    }

    // Handle directory-only: trailing /
    if (!line.empty() && line.back() == '/') {
      rule.isDirectoryOnly = true;
      line.pop_back();
      if (line.empty()) {
        continue;
      }
    }

    // Determine if pattern is anchored.
    // A pattern is anchored if it contains a slash (other than trailing,
    // which was already removed above).
    // Leading slash means anchored to the .gitignore directory.
    if (line.find('/') != std::string::npos) {
      rule.isAnchored = true;
      // Remove leading slash if present (it's just an anchor marker)
      if (line[0] == '/') {
        line = line.substr(1);
      }
    }

    rule.pattern = line;
    rules.push_back(rule);
  }

  return rules;
}

bool GitignoreParser::matchPattern(
  const std::string &pattern,
  const std::string &relativePath,
  bool caseSensitive
) {
  unsigned int flags = WM_PATHNAME;
  if (!caseSensitive) {
    flags |= WM_CASEFOLD;
  }

  return wildmatch(pattern.c_str(), relativePath.c_str(), flags) == WM_MATCH;
}

