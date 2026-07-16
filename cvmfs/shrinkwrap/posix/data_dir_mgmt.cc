/**
 * This file is part of the CernVM File System.
 */
#include "data_dir_mgmt.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "helpers.h"
#include "shrinkwrap/fs_traversal_interface.h"
#include "util/logging.h"
#include "util/posix.h"

/**
 * Method which recursively creates the .data subdirectories below (and
 * including) cur_path, starting at the given depth.
 *
 * Returns false (and logs the reason) on the first directory that could
 * not be created, instead of aborting the process.
 */
bool PosixCheckDirStructure(std::string cur_path, mode_t mode, unsigned depth) {
  std::string max_dir_name = std::string(kDigitsPerDirLevel, 'f');
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

namespace {

/**
 * Per-thread context used to fan out the creation of the top-level
 * .data subdirectories (and their sub-trees) across several worker threads.
 * The `range_size` top-level directories are distributed round-robin over
 * `thread_total` threads, each thread handling every `thread_total`-th
 * directory starting at `thread_num`.
 */
struct DirCreationThreadContext {
  std::string base_path;
  mode_t mode;
  unsigned thread_num;
  unsigned thread_total;
  unsigned range_size;
  bool success;
};

void *PosixCheckDirStructureWorker(void *data) {
  DirCreationThreadContext *tc =
      reinterpret_cast<DirCreationThreadContext *>(data);
  tc->success = true;
  char hex[kDigitsPerDirLevel + 1];
  char dir_name_template[5];
  snprintf(dir_name_template,
           sizeof(dir_name_template),
           "%%%02ux",
           kDigitsPerDirLevel);
  for (unsigned i = tc->thread_num; i < tc->range_size;
       i += tc->thread_total) {
    snprintf(hex, sizeof(hex), dir_name_template, i);
    std::string this_path = tc->base_path + "/" + std::string(hex);
    int res = mkdir(this_path.c_str(), tc->mode);
    if (res != 0 && errno != EEXIST) {
      LogCvmfs(kLogCvmfs, kLogStderr,
               "Failed to create data directory '%s': %s",
               this_path.c_str(), strerror(errno));
      tc->success = false;
      return NULL;
    }
    if (!PosixCheckDirStructure(this_path, tc->mode, 2)) {
      tc->success = false;
      return NULL;
    }
  }
  return NULL;
}

}  // anonymous namespace

/**
 * Initializes the .data directory with all subdirectories.
 *
 * The top-level directory is created first (sequentially), then the
 * (up to 16^(2*kDigitsPerDirLevel)) top-level subdirectories and their
 * sub-trees are created in parallel across the number of threads requested
 * for this context, since they are fully independent of one another.
 */
bool InitializeDataDirectory(struct fs_traversal_context *ctx) {
  const mode_t mode = 0700;
  if (!MkdirDeep(ctx->data, mode)) {
    LogCvmfs(kLogCvmfs, kLogStderr,
             "Failed to create data directory '%s': %s",
             ctx->data, strerror(errno));
    return false;
  }

  if (kDirLevels == 0)
    return true;

  std::string base_path = ctx->data;
  std::string max_dir_name = std::string(kDigitsPerDirLevel, 'f');
  if (DirectoryExists(base_path + "/" + max_dir_name)) {
    // Top level already fully created (e.g. when resuming a previous,
    // interrupted run): just verify/complete the sub-tree sequentially.
    return PosixCheckDirStructure(base_path + "/" + max_dir_name, mode, 2);
  }

  struct fs_traversal_posix_context *posix_ctx =
      reinterpret_cast<struct fs_traversal_posix_context *>(ctx->ctx);
  const unsigned range_size = ((unsigned)1) << (4 * kDigitsPerDirLevel);
  unsigned thread_total = posix_ctx->num_threads;
  if (thread_total < 1)
    thread_total = 1;
  if (thread_total > range_size)
    thread_total = range_size;

  std::vector<DirCreationThreadContext> thread_ctxs(thread_total);
  for (unsigned i = 0; i < thread_total; i++) {
    thread_ctxs[i].base_path = base_path;
    thread_ctxs[i].mode = mode;
    thread_ctxs[i].thread_num = i;
    thread_ctxs[i].thread_total = thread_total;
    thread_ctxs[i].range_size = range_size;
    thread_ctxs[i].success = true;
  }

  if (thread_total > 1) {
    std::vector<pthread_t> workers(thread_total);
    for (unsigned i = 0; i < thread_total; i++) {
      int retval = pthread_create(&workers[i], NULL,
                                  PosixCheckDirStructureWorker,
                                  &thread_ctxs[i]);
      assert(retval == 0);
    }
    for (unsigned i = 0; i < thread_total; i++)
      pthread_join(workers[i], NULL);
  } else {
    PosixCheckDirStructureWorker(&thread_ctxs[0]);
  }

  bool success = true;
  for (unsigned i = 0; i < thread_total; i++)
    success = success && thread_ctxs[i].success;
  return success;
}
