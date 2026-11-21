/* S09 宿主测试小工具：临时树清理 + 经 FsOps 播种文件。 */
#ifndef S09_TEST_UTIL_H
#define S09_TEST_UTIL_H

#include "test.h"
#include "Contract/FsOps.h"
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static inline void s09_rmtree (const char *path) {
  DIR *d = opendir (path);
  struct dirent *de;
  char child[1024];
  struct stat sb;
  if (d == NULL) return;
  while ((de = readdir (d)) != NULL) {
    if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0) continue;
    snprintf (child, sizeof (child), "%s/%s", path, de->d_name);
    if (lstat (child, &sb) == 0 && S_ISDIR (sb.st_mode)) s09_rmtree (child);
    else unlink (child);
  }
  closedir (d);
  rmdir (path);
}

static inline void s09_seed (FS_OPS *fs, CONST CHAR16 *path, CONST char *text) {
  FS_HANDLE *h = NULL;
  UINTN n = strlen (text);
  ASSERT_OK (fs->Open (fs, path, FS_MODE_WRITE | FS_MODE_CREATE, &h));
  ASSERT_OK (fs->Write (fs, h, text, &n));
  ASSERT_OK (fs->Close (fs, h));
}

#endif
