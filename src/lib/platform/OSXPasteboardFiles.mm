/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXPasteboardFiles.h"

#import <AppKit/AppKit.h>

namespace {

NSPasteboard *pasteboardOf(void *pasteboard)
{
  return static_cast<NSPasteboard *>(pasteboard);
}

NSDictionary *fileURLsOnly()
{
  return @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
}

} // namespace

OSXPasteboardFiles::OSXPasteboardFiles(bool ownPasteboard) : m_own(ownPasteboard)
{
  @autoreleasepool {
    NSPasteboard *pasteboard =
        ownPasteboard ? [NSPasteboard pasteboardWithUniqueName] : [NSPasteboard generalPasteboard];
    m_pasteboard = [pasteboard retain];
  }
}

OSXPasteboardFiles::OSXPasteboardFiles(const std::string &name, bool releaseWhenDone) : m_own(releaseWhenDone)
{
  @autoreleasepool {
    m_pasteboard = [[NSPasteboard pasteboardWithName:[NSString stringWithUTF8String:name.c_str()]] retain];
  }
}

OSXPasteboardFiles::~OSXPasteboardFiles()
{
  @autoreleasepool {
    NSPasteboard *pasteboard = pasteboardOf(m_pasteboard);
    if (m_own) {
      [pasteboard releaseGlobally];
    }
    [pasteboard release];
  }
}

bool OSXPasteboardFiles::read(std::vector<std::string> &paths) const
{
  paths.clear();
  @autoreleasepool {
    NSPasteboard *pasteboard = pasteboardOf(m_pasteboard);
    if (![pasteboard canReadObjectForClasses:@[ NSURL.class ] options:fileURLsOnly()]) {
      return false;
    }
    NSArray *urls = [pasteboard readObjectsForClasses:@[ NSURL.class ] options:fileURLsOnly()];
    for (NSURL *url in urls) {
      // Finder refers to a file by its number on the volume, this gives its path
      NSURL *path = url.filePathURL;
      const char *representation = (path != nil) ? path.fileSystemRepresentation : nullptr;
      if (representation != nullptr && representation[0] == '/') {
        paths.emplace_back(representation);
      }
    }
  }
  return !paths.empty();
}

long OSXPasteboardFiles::write(const std::vector<std::string> &paths, const std::vector<bool> &isDirectory)
{
  @autoreleasepool {
    NSMutableArray *urls = [NSMutableArray arrayWithCapacity:paths.size()];
    for (size_t i = 0; i < paths.size(); ++i) {
      NSString *path = [NSString stringWithUTF8String:paths[i].c_str()];
      if (path == nil) {
        continue;
      }
      // told whether it is a directory, the URL is made without looking at the file
      const bool directory = (i < isDirectory.size()) && isDirectory[i];
      [urls addObject:[NSURL fileURLWithPath:path isDirectory:directory ? YES : NO]];
    }
    if (urls.count == 0) {
      return -1;
    }

    // what the system writes for a file URL depends on the file: it looks at each one
    NSPasteboard *pasteboard = pasteboardOf(m_pasteboard);
    [pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
    if (![pasteboard writeObjects:urls]) {
      return -1;
    }
    return static_cast<long>(pasteboard.changeCount);
  }
}

long OSXPasteboardFiles::changeCount() const
{
  @autoreleasepool {
    return static_cast<long>(pasteboardOf(m_pasteboard).changeCount);
  }
}

void OSXPasteboardFiles::clear()
{
  @autoreleasepool {
    [pasteboardOf(m_pasteboard) clearContents];
  }
}

void OSXPasteboardFiles::writeText(const std::string &text)
{
  @autoreleasepool {
    NSPasteboard *pasteboard = pasteboardOf(m_pasteboard);
    [pasteboard clearContents];
    [pasteboard setString:[NSString stringWithUTF8String:text.c_str()] forType:NSPasteboardTypeString];
  }
}

void OSXPasteboardFiles::writeFileReferences(const std::vector<std::string> &paths)
{
  @autoreleasepool {
    NSMutableArray *urls = [NSMutableArray arrayWithCapacity:paths.size()];
    for (const auto &path : paths) {
      NSURL *reference = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]].fileReferenceURL;
      if (reference != nil) {
        [urls addObject:reference];
      }
    }
    NSPasteboard *pasteboard = pasteboardOf(m_pasteboard);
    [pasteboard clearContents];
    [pasteboard writeObjects:urls];
  }
}
