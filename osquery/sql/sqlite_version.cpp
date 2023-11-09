/**
 * Copyright (c) 2014-present, The osquery authors
 *
 * This source code is licensed as defined by the LICENSE file found in the
 * root directory of this source tree.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
 */

#include <string>
#include <unordered_set>

#include <sqlite3.h>

namespace osquery {
/**
 * @brief Compares the position of epoch delimiter between two versions.
 * Return difference of epoch segment length if both versions have epoch.
 * Return 1 if left version has epoch but not the right.
 * Return -1 if right version has epoch but not the left.
 * Return 0 if epoch doesn't exist in either version.
 */
static int compareEpoch(const std::string_view& l_ver,
                        const std::string_view& r_ver) {
  auto l_epoch = l_ver.find(":");
  auto r_epoch = r_ver.find(":");

  if (l_epoch != l_ver.npos && r_epoch != r_ver.npos) {
    return l_epoch - r_epoch;
  } else if (l_epoch != l_ver.npos) {
    return 1;
  } else if (r_epoch != r_ver.npos) {
    return -1;
  }

  return 0;
}

/**
 * @brief Check if character is defined as a delimiter and return the delimiter
 * precedence.
 */
static int delimiterPrecedence(char c) {
  switch (c) {
  case '~':
    return 1;
  case '-':
    return 2;
  case '^':
    return 3;
  case '.':
    return 4;
  case ':':
    return 5;
  default:
    return 0;
  }
}

/**
 * @brief Return remainder sort order, or return the version length difference.
 */
static int compareRemainder(const std::string_view& l_ver,
                            const std::string_view& r_ver,
                            size_t& pos,
                            int& diff,
                            const bool& comp_remaining,
                            const bool& remainder_precedence) {
  // This supports linux package versioning sort order when a tilde should be
  // less than, a caret should be greater than, and a hyphen should be equal.
  //
  // When remainder_precedence = true, this will return the segment value diff
  // if the last compared character wasn't numeric, otherwise it falls through
  // to return length diff.
  //
  // When remainder_precedence = false, this will return the segment value diff
  // if there is any, before falling through to return length diff.
  if (comp_remaining) {
    if (l_ver.size() == pos) {
      switch (r_ver[pos]) {
      case '~':
        return 1;
      case '-':
        return 0;
      case '^':
        return -1;
      default:
        if (diff != 0 && (!remainder_precedence || !isdigit(l_ver[pos - 1]))) {
          return diff;
        }
      }
    }

    if (r_ver.size() == pos) {
      switch (l_ver[pos]) {
      case '~':
        return -1;
      case '-':
        return 0;
      case '^':
        return 1;
      default:
        if (diff != 0 && (!remainder_precedence || !isdigit(r_ver[pos - 1]))) {
          return diff;
        }
      }
    }
  }

  return l_ver.size() - r_ver.size();
}

/**
 * @brief Compares two versions strings against each other.
 * Return 0 if the versions should evaluate as equal.
 * Return negative int if the left string is less than the right.
 * Return positive int if the left string is greater than the right.
 *
 * This accepts options `epoch`, `delim_precedence`, `comp_remaining`, and
 * `remainder_precedence` mostly for linux package versioning support.
 *
 * epoch: If true, versions with an epoch value always sort greater than
 * something without an epoch.
 *
 * delim_precedence: If true, version delimiters will compare to each other to
 * determine sort order.
 *
 * comp_remaining: If true, and there is remaining length to one of the
 * versions, then instead of only returning the length difference, this signals
 * to compute sort order in the remaining content.
 *
 * remainder_precedence: If true, and the remaining content isn't clear on sort
 * order, then only return value difference if the last compared character isn't
 * numeric, if it is then always return length difference.
 */
static int versionCompare(int l_len,
                          const void* l_version,
                          int r_len,
                          const void* r_version,
                          const bool epoch = false,
                          const bool delim_precedence = false,
                          const bool comp_remaining = false,
                          const bool remainder_precedence = false) {
  // Early return if one of the version strings is empty.
  if (l_len == 0 && r_len == 0) {
    return 0;
  } else if (l_len == 0) {
    return -1;
  } else if (r_len == 0) {
    return 1;
  }

  const std::string_view l_ver(static_cast<const char*>(l_version), l_len);
  const std::string_view r_ver(static_cast<const char*>(r_version), r_len);

  // Early return if versions are equal.
  if (l_ver == r_ver) {
    return 0;
  }

  // Check for and return difference in epoch position.
  if (epoch) {
    auto epoch_diff = compareEpoch(l_ver, r_ver);
    if (epoch_diff != 0) {
      return epoch_diff;
    }
  }

  int first_diff = 0;
  size_t min = std::min(l_len, r_len);
  for (size_t i = 0; i < min; i++) {
    auto l_delim = delimiterPrecedence(l_ver[i]);
    auto r_delim = delimiterPrecedence(r_ver[i]);

    // Until we hit a delimiter, we will compare the ASCII values of each
    // character, and store the first difference of this segment.
    if (l_delim == 0 && r_delim == 0) {
      first_diff = first_diff == 0 ? int(l_ver[i]) - int(r_ver[i]) : first_diff;
      continue;
    } else if (l_delim == 0) {
      return 1;
    } else if (r_delim == 0) {
      return -1;
    }

    // If we've hit delimiters in both versions, then return the first value
    // difference in this segment.
    if (first_diff != 0) {
      return first_diff;
    }

    // Check for and return difference in delimiter precedence.
    if (delim_precedence) {
      auto delim_diff = l_delim - r_delim;
      if (delim_diff != 0) {
        return delim_diff;
      }
    }
  }

  // If the versions are the same length, then return the first difference in
  // the final segment.
  if (l_len == r_len) {
    return first_diff;
  }

  return compareRemainder(
      l_ver, r_ver, min, first_diff, comp_remaining, remainder_precedence);
}

/**
 * @brief versionCompare wrapper for a sqlite function.
 */
static void versionCompareFunc(sqlite3_context* context,
                               int argc,
                               sqlite3_value** argv) {
  if (argc < 3) {
    sqlite3_result_error(
        context,
        "Must provide two version strings and an operator to compare.",
        -1);
    return;
  }

  if (sqlite3_value_type(argv[0]) != SQLITE_TEXT ||
      sqlite3_value_type(argv[1]) != SQLITE_TEXT ||
      sqlite3_value_type(argv[2]) != SQLITE_TEXT) {
    sqlite3_result_error(
        context,
        "Must provide two version strings and an operator to compare.",
        -1);
    return;
  }

  std::unordered_set<std::string> ops = {"<", "<=", "=", ">=", ">"};
  const char* op(reinterpret_cast<const char*>(sqlite3_value_text(argv[1])));

  if (ops.find(op) == ops.end()) {
    sqlite3_result_error(context,
                         "Unknown compare operator. Must provide one of the "
                         "following: (<, <=, =, >=, >)",
                         -1);
    return;
  }

  std::vector<bool> options(4);

  for (auto i = 3; i < argc && i < 7; i++) {
    if (sqlite3_value_type(argv[i]) != SQLITE_INTEGER &&
        sqlite3_value_type(argv[i]) != SQLITE_NULL) {
      sqlite3_result_error(
          context,
          "Options for epoch, delim_precedence, comp_remaining, and "
          "remainder_precedence must be true, false, or null.",
          -1);
      return;
    } else {
      options[i - 3] = sqlite3_value_int(argv[i]);
    }
  }

  bool result;
  const char* l(reinterpret_cast<const char*>(sqlite3_value_text(argv[0])));
  const char* r(reinterpret_cast<const char*>(sqlite3_value_text(argv[2])));
  auto rc = versionCompare(strlen(l),
                           (const void*)l,
                           strlen(r),
                           (const void*)r,
                           options[0],
                           options[1],
                           options[2],
                           options[3]);

  if (rc < 0) {
    result = op[0] == '<';
  } else if (rc > 0) {
    result = op[0] == '>';
  } else {
    result = (op[0] == '=' || strlen(op) == 2);
  }

  sqlite3_result_int(context, result);
}

/**
 * @brief Collate generic version strings.
 */
static int versionCollate(
    void* notUsed, int nKey1, const void* pKey1, int nKey2, const void* pKey2) {
  (void)notUsed;
  return versionCompare(nKey1, pKey1, nKey2, pKey2, false, false, false, false);
}

/**
 * @brief Collate arch package version strings.
 */
static int versionCollateARCH(
    void* notUsed, int nKey1, const void* pKey1, int nKey2, const void* pKey2) {
  (void)notUsed;
  return versionCompare(nKey1, pKey1, nKey2, pKey2, true, false, true, true);
}

/**
 * @brief Collate deb package version strings.
 */
static int versionCollateDPKG(
    void* notUsed, int nKey1, const void* pKey1, int nKey2, const void* pKey2) {
  (void)notUsed;
  return versionCompare(nKey1, pKey1, nKey2, pKey2, true, true, false, false);
}

/**
 * @brief Collate rhel package version strings.
 */
static int versionCollateRHEL(
    void* notUsed, int nKey1, const void* pKey1, int nKey2, const void* pKey2) {
  (void)notUsed;
  return versionCompare(nKey1, pKey1, nKey2, pKey2, true, true, true, false);
}

void registerVersionExtensions(sqlite3* db) {
  sqlite3_create_function(db,
                          "version_compare",
                          -1,
                          SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                          nullptr,
                          versionCompareFunc,
                          nullptr,
                          nullptr);
  sqlite3_create_collation(db, "version", SQLITE_UTF8, nullptr, versionCollate);
  sqlite3_create_collation(
      db, "version_arch", SQLITE_UTF8, nullptr, versionCollateARCH);
  sqlite3_create_collation(
      db, "version_dpkg", SQLITE_UTF8, nullptr, versionCollateDPKG);
  sqlite3_create_collation(
      db, "version_rhel", SQLITE_UTF8, nullptr, versionCollateRHEL);
}
} // namespace osquery
