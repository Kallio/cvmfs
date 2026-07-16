/**
 * This file is part of the CernVM File System.
 */
#include "data_dir_mgmt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "helpers.h"
#include "shrinkwrap/fs_traversal_interface.h"
#include "util/logging.h"
#include "util/posix.h"

/**
 * Method which recursively creates the .data subdirectories
 *
 * Returns false (and logs the reason) on the first directory that could
 * not be created, instead of aborting the process.
 */
bool PosixCheckDirStructure(std::string cur_path,
                            mode_t mode,
                            unsigned depth = 1) {
  std::string max_dir_name = std::string(kDigitsPerDirLevel, 'f');
  // Build current base path
  if (depth == 1) {
    if (!MkdirDeep(cur_path.c_str(), mode)) {
      LogCvmfs(kLogCvmfs, kLogStderr,
               "Failed to create data directory '%s': %s",
               cur_path.c_str(), strerror(errno));
      return false;
    }
  } else {
    int res = mkdir(cur_path.c_str(), mode);
    if (res != 0 && errno != EEXIST) {
      LogCvmfs(kLogCvmfs, kLogStderr,
               "Failed to create data directory '%s': %s",
               cur_path.c_str(), strerror(errno));
      return false;
    }
  }
  // Build template for directory names:
  assert(kDigitsPerDirLevel <= 99);
  char hex[kDigitsPerDirLevel + 1];
  char dir_name_template[5];
  snprintf(dir_name_template,
           sizeof(dir_name_template),
           "%%%02ux",
           kDigitsPerDirLevel);
  // Go through all levels:
  for (; depth <= kDirLevels; depth++) {
    if (!DirectoryExists(cur_path + "/" + max_dir_name)) {
      // Directories in this level not yet fully created...
      for (unsigned int i = 0;
           i < (((unsigned int)1) << 4 * kDigitsPerDirLevel);
           i++) {
        // Go through directories 0^kDigitsPerDirLevel to f^kDigitsPerDirLevel
        snprintf(hex, sizeof(hex), dir_name_template, i);
        std::string this_path = cur_path + "/" + std::string(hex);
        int res = mkdir(this_path.c_str(), mode);
        if (res != 0 && errno != EEXIST) {
          LogCvmfs(kLogCvmfs, kLogStderr,
                   "Failed to create data directory '%s': %s",
                   this_path.c_str(), strerror(errno));
          return false;
        }
        // Once directory created: Prepare substructures
        if (!PosixCheckDirStructure(this_path, mode, depth + 1))
          return false;
      }
      break;
    } else {
      // Directories on this level fully created; check ./
      if (!PosixCheckDirStructure(cur_path + "/" + max_dir_name, mode,
                                  depth + 1))
        return false;
    }
  }
  return true;
}

bool InitializeDataDirectory(struct fs_traversal_context *ctx) {
  // NOTE(steuber): Can we do this in parallel?
  return PosixCheckDirStructure(ctx->data, 0700);
}
