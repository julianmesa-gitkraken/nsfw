#ifndef GITIGNORE_PARSER_H
#define GITIGNORE_PARSER_H

#include <string>
#include <vector>

struct GitignoreRule {
  std::string pattern;     // The cleaned pattern (without leading ! or trailing /)
  bool isNegation;         // Started with !
  bool isDirectoryOnly;    // Ended with /
  bool isAnchored;         // Contains / (not just trailing)
};

class GitignoreParser {
public:
  /**
   * Parse .gitignore file content into a list of rules.
   *
   * Handles:
   * - Leading # → comment (skip)
   * - Leading \# → escaped hash (literal #)
   * - Trailing spaces → stripped unless escaped with backslash
   * - Empty lines → skip
   * - !pattern → negation (re-include)
   * - pattern/ → directory-only match
   * - Patterns containing / (not just trailing) → anchored to .gitignore location
   * - Patterns without / → match at any depth
   */
  static std::vector<GitignoreRule> parse(const std::string &content);

  /**
   * Check if a relative path matches a single pattern.
   * Uses wildmatch() internally with WM_PATHNAME flag.
   *
   * @param pattern The gitignore pattern to match against
   * @param relativePath The path relative to the .gitignore location
   * @param caseSensitive Whether matching should be case-sensitive (default: true)
   * @return true if the path matches the pattern
   */
  static bool matchPattern(
    const std::string &pattern,
    const std::string &relativePath,
    bool caseSensitive = true
  );
};

#endif

