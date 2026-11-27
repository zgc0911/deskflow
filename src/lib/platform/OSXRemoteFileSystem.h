/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardFileList.h"
#include "platform/OSXNfsServer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace deskflow {

class RemoteFiles;

//! The files that other computers have copied, as a tree of read-only directories
/*!
The root holds one directory for each list of copied files, named after the
identifier of the list, and in it the files and directories that were copied.
What is inside a copied directory is asked from the other computer when it is
first looked into, and the contents of a file when they are read. Nothing is
transferred for files that are never pasted.

A file is downloaded in order, starting at the position of the first read.
Reads may come several at a time and a little out of order, they are answered
from what has arrived and wait for what has not. A read from an entirely
different position starts another download.

Time passes between copying and pasting, so sizes and dates are asked for
again when they are looked at after a while. A file that was changed after
it was copied is pasted as it is when it is pasted. Right after a list was
added its sizes and dates are taken as they are: they were current when the
clipboard was sent.

Called from the threads of the NFS server, any number at a time.
*/
class OSXRemoteFileSystem : public nfs::IFileSystem
{
public:
  //! Sizes and times that decide how reading behaves, tests shorten them
  struct Limits
  {
    //! Lists of copied files that are kept, the oldest is dropped for a new one
    size_t offers = 4;
    //! Downloads that may run at once
    size_t streams = 12;
    //! Directory listings that may be asked for at once
    int listings = 3;
    //! Room the other computer is given for file data that is on its way
    uint32_t window = 2 * 1024 * 1024;
    //! How far a download runs ahead of what was asked for
    uint64_t readAhead = 2 * 1024 * 1024;
    //! Data that is kept for a while after it was read, for a reader that asks again
    uint64_t keepBehind = 2 * 1024 * 1024;
    //! Most data a download holds on to for readers that are behind
    uint64_t span = 24 * 1024 * 1024;
    //! A read that has part of its data waits this long for the rest
    int partialWaitMs = 1000;
    //! Longest that a call is kept waiting before it is told to come back later
    int waitMs = 30000;
    //! Times in a row that a call is told to come back later, after that it fails
    int retries = 3;
    //! A download that nobody reads from is given up after this long
    int idleMs = 20000;
    //! Sizes and dates are asked for again when they are older than this and nothing is being read
    int freshMs = 3000;
    //! Longest that a call waits for sizes and dates to be asked for again
    int refreshWaitMs = 3000;
    //! How long a failure is remembered, so that everything that waited for the same thing fails with it
    int failureMs = 5000;
  };

  explicit OSXRemoteFileSystem(std::shared_ptr<RemoteFiles> files);
  OSXRemoteFileSystem(std::shared_ptr<RemoteFiles> files, const Limits &limits);
  ~OSXRemoteFileSystem() override;
  OSXRemoteFileSystem(OSXRemoteFileSystem const &) = delete;
  OSXRemoteFileSystem &operator=(OSXRemoteFileSystem const &) = delete;

  //! Make the files of a list available
  /*!
  Returns the path of each entry of the list below the root, with "/" between
  the two components. An entry whose name can not be used here has an empty
  path. Adding a list that was added before changes nothing.
  */
  std::vector<std::string> addOffer(const ClipboardFileList &list);

  //! Fail every call from now on, wake the ones that wait and stop all downloads
  /*!
  Waits at most \p waitMs for the threads that download and list to end,
  and returns whether they did. A listing ends when the other computer
  answers or the connection is gone.
  */
  bool shutdown(int waitMs = 3000);

  //! Fail the calls that wait right now, for a volume that was unmounted. Later calls work as before.
  void interrupt();

  //! @name for tests
  //@{

  //! Bytes of file contents received from other computers so far
  uint64_t fetchedBytes() const;
  //! Number of listings asked from other computers so far
  uint64_t listingCount() const;
  //! Downloads that are held at the moment, running or not
  size_t streamCount() const;
  //! Threads that download or list at the moment
  int threadCount() const;

  //@}

  // nfs::IFileSystem overrides
  nfs::FileHandle rootHandle() override;
  nfs::Status getAttributes(const nfs::FileHandle &handle, nfs::Attributes &attributes) override;
  nfs::Status lookup(
      const nfs::FileHandle &directory, const std::string &name, nfs::FileHandle &handle, nfs::Attributes &attributes
  ) override;
  nfs::Status readDirectory(
      const nfs::FileHandle &directory, uint64_t first, size_t limit, std::vector<nfs::DirectoryEntry> &entries,
      bool &complete
  ) override;
  nfs::Status
  read(const nfs::FileHandle &file, uint64_t offset, uint32_t count, std::string &data, bool &endOfFile) override;

private:
  struct State;

  //! Shared with the threads that download and list, they may outlive this object for a moment
  std::shared_ptr<State> m_state;
};

} // namespace deskflow
