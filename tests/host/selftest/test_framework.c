/* S00 框架自检：验证 test.h + EfiCompat + EfiMem + MockTui + MockFs 宿主可用。 */
#include "test.h"
#include "MockTui.h"
#include "MockFs.h"

TEST (compat_types) {
  ASSERT_EQ (sizeof (CHAR16), 2);
  ASSERT_EQ (sizeof (UINTN), sizeof (void *));
  ASSERT_TRUE (EFI_ERROR (EFI_NOT_FOUND));
  ASSERT_FALSE (EFI_ERROR (EFI_SUCCESS));
  ASSERT_EQ (EfiStrCmp (U16("abc"), U16("abc")), 0);
  ASSERT_EQ (EfiStrLen (U16("abc")), 3);
  ASSERT_TRUE (EfiStrChr (U16("a/b"), L'/') != NULL);
}

TEST (mem_alloc) {
  UINT8 *p = MemAlloc (16);
  ASSERT_TRUE (p != NULL);
  ASSERT_EQ (p[0], 0);
  p[0] = 42;
  p = MemRealloc (p, 32);
  ASSERT_EQ (p[0], 42);
  MemFree (p);
}

TEST (mock_tui) {
  CHAR16 row[128];
  MockTuiReset ();
  MockTuiSetSize (25, 80);
  ASSERT_OK (TuiInit ());
  TuiPutText (0, 0, U16("EasyEFI"), TUI_ATTR_TITLE);
  MockTuiRowText (0, row, 128);
  ASSERT_EQ (row[0], L'E');
  ASSERT_EQ (MockTuiGetCell (0, 0).Attr, TUI_ATTR_TITLE);
  MockTuiPushKey (SCAN_ESC, 0);
  TUI_KEY k;
  ASSERT_OK (TuiWaitKey (&k));
  ASSERT_EQ (TuiKeyToCommand (&k), CMD_ESC);
  ASSERT_ERR (TuiWaitKey (&k));
}

TEST (mock_fs) {
  FS_OPS *fs = MockFsNew ();
  FS_ENTRY *ents;
  UINTN n, len;
  UINT8 buf[8];
  FS_HANDLE *h;
  MockFsAddDir (fs, U16("scripts"));
  MockFsAddFile (fs, U16("scripts/fpt.nsh"), (CONST UINT8 *)"echo hi", 7);
  ASSERT_OK (fs->Enum (fs, U16(""), &ents, &n));
  ASSERT_EQ (n, 1);
  ASSERT_TRUE (ents[0].IsDir);
  MemFree (ents);
  ASSERT_OK (fs->Open (fs, U16("scripts/fpt.nsh"), FS_MODE_READ, &h));
  len = sizeof (buf);
  ASSERT_OK (fs->Read (fs, h, buf, &len));
  ASSERT_EQ (len, 7);
  ASSERT_MEM_EQ (buf, "echo hi", 7);
  ASSERT_OK (fs->Close (fs, h));
  ASSERT_OK (fs->Delete (fs, U16("scripts/fpt.nsh")));
  ASSERT_ERR (fs->Open (fs, U16("scripts/fpt.nsh"), FS_MODE_READ, &h));
  fs->Destroy (fs);
}

int main (void) {
  RUN_TEST (compat_types);
  RUN_TEST (mem_alloc);
  RUN_TEST (mock_tui);
  RUN_TEST (mock_fs);
  RUN_TESTS_END ();
}
