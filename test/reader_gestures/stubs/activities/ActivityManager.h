#pragma once

class ActivityManager {
 public:
  bool openedFileBrowser = false;
  bool yourBooksReturnContext = false;

  void goToFileBrowser(const char*) { openedFileBrowser = true; }
  bool hasYourBooksReturnContext() const { return yourBooksReturnContext; }
};
