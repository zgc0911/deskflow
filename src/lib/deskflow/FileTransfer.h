/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardFileList.h"
#include "deskflow/FileOfferRegistry.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class Event;
class IEventQueue;

namespace deskflow {

//! Connection to another computer that file transfer messages are sent over
/*!
Implemented by the server and client proxies. Every method is called on the
thread that runs the event queue.
*/
class IFileTransferChannel
{
public:
  virtual ~IFileTransferChannel() = default;
  virtual void sendFileRequest(uint32_t requestId, uint8_t kind, const std::string &arguments) = 0;
  virtual void sendFileResponse(uint32_t requestId, uint8_t mark, const std::string &content) = 0;
  virtual void sendFileCredit(uint32_t requestId, uint32_t bytes) = 0;
  virtual void sendFileCancel(uint32_t requestId) = 0;
};

//! Outcome of asking another computer for a listing or for file contents
enum class FileTransferStatus
{
  Ok,
  Cancelled,    //!< The request was abandoned by this computer
  Disconnected, //!< The other computer is gone, or no longer known for these files
  TimedOut,     //!< The other computer stopped answering
  Refused,      //!< File sharing is turned off, here or there
  NotFound,     //!< The files are no longer offered, or the path does not exist
  Failed        //!< Anything else, such as a read error on the other computer
};

class FileTransfer;
class RemoteFiles;
struct FileTransferRequest;
struct FileTransferShared;

//! Reads the contents of a file that is on another computer
/*!
Created by FileTransfer::openFile(). Reading blocks, so a reader must not be
used on the thread that runs the event queue. One reader is used by one
thread at a time, cancel() may be called from any thread.
*/
class RemoteFileReader
{
public:
  ~RemoteFileReader();
  RemoteFileReader(RemoteFileReader const &) = delete;
  RemoteFileReader &operator=(RemoteFileReader const &) = delete;

  //! Read the next bytes of the file
  /*!
  Blocks until at least one byte is available, the file ends or the transfer
  fails. At the end of the file the status is Ok and \p bytesRead is zero.
  */
  FileTransferStatus read(void *buffer, uint32_t size, uint32_t &bytesRead);

  //! Give up, a read that is blocked returns Cancelled
  void cancel();

private:
  friend class RemoteFiles;
  RemoteFileReader(std::shared_ptr<FileTransferShared> shared, std::shared_ptr<FileTransferRequest> request);

  std::shared_ptr<FileTransferShared> m_shared;
  std::shared_ptr<FileTransferRequest> m_request;
};

//! Access to the files that another computer has copied
/*!
Obtained from FileTransfer::remoteFiles(). The methods block and may be
called from any thread except the one that runs the event queue. The object
may outlive the FileTransfer it came from, requests then fail with
Disconnected.
*/
class RemoteFiles
{
public:
  RemoteFiles(RemoteFiles const &) = delete;
  RemoteFiles &operator=(RemoteFiles const &) = delete;

  //! List a directory on the other computer
  /*!
  Blocks until the listing is complete. \p item is the index of a top level
  entry of the list \p offerId, \p relativePath a path below it with "/"
  between components (empty for the entry itself).
  */
  FileTransferStatus listDirectory(
      uint64_t offerId, uint32_t item, const std::string &relativePath, std::vector<ClipboardFileEntry> &entries
  );

  //! Ask what became of the files and directories that were copied
  /*!
  Blocks until the answer is complete. \p entries are the top level entries
  of the list \p offerId, in the order of the list and as they are now: a
  copied file may have changed since, or it may be gone, which is marked.
  */
  FileTransferStatus listOffer(uint64_t offerId, std::vector<ClipboardFileEntry> &entries);

  //! Open a file on the other computer for reading
  /*!
  Never returns nullptr, a failure is reported by the first read. \p window
  is how many bytes may be on their way or waiting in the reader at any time.
  */
  std::unique_ptr<RemoteFileReader>
  openFile(uint64_t offerId, uint32_t item, const std::string &relativePath, uint64_t offset, uint32_t window = 0);

private:
  friend class FileTransfer;
  explicit RemoteFiles(std::shared_ptr<FileTransferShared> shared);

  std::shared_ptr<FileTransferShared> m_shared;
};

//! Shares the files on the clipboard with other computers
/*!
One object serves both directions. For files copied on this computer it
keeps the list that other computers are told about and answers their
requests for listings and contents. For files copied on another computer it
offers a blocking interface to list directories and read files, which the
platform specific paste code uses when the files are actually pasted.

Unless noted otherwise methods must be called on the thread that runs the
event queue. All sending and receiving happens there, in small pieces, so
that input events keep flowing while a file is transferred.
*/
class FileTransfer
{
public:
  //! Request kinds, marks and sizes of the file transfer messages
  struct Protocol
  {
    inline static const uint8_t RequestList = 1;
    inline static const uint8_t RequestRead = 2;
    //! Instead of the index of an entry in a request to list: the entries of the list themselves
    inline static const uint32_t AllItems = 0xffffffff;
    inline static const uint8_t MarkData = 1;
    inline static const uint8_t MarkEnd = 2;
    inline static const uint8_t MarkError = 3;
    //! Most file bytes in one message
    inline static const uint32_t ChunkSize = 64 * 1024;
    //! Room a reader grants for file data by default
    inline static const uint32_t DefaultWindow = 1024 * 1024;
    //! Most room a reader may ask for
    inline static const uint32_t MaxWindow = 16 * 1024 * 1024;
    //! Longest path below a top level entry, in bytes
    inline static const size_t MaxPathLength = 0xffff;
  };

  //! Error codes sent to the other computer
  enum class Error : uint32_t
  {
    Disabled = 1,     //!< File sharing is turned off
    UnknownOffer = 2, //!< The file list is not offered (any more)
    BadPath = 3,      //!< The request does not name an offered file
    NotFound = 4,     //!< Nothing exists at the path
    WrongType = 5,    //!< A directory was read or a file was listed
    ReadFailed = 6,   //!< The file could not be opened or read
    Busy = 7          //!< Too many requests at once
  };

  explicit FileTransfer(IEventQueue *events);
  ~FileTransfer();
  FileTransfer(FileTransfer const &) = delete;
  FileTransfer &operator=(FileTransfer const &) = delete;

  //! Turn file sharing on or off
  void setEnabled(bool enabled);
  bool isEnabled() const;

  //! @name files copied on this computer
  //@{

  //! Offer the files on the local clipboard
  /*!
  \p paths are absolute local paths in UTF-8. Returns the clipboard data for
  IClipboard::Format::FileList, or an empty string if there is nothing to
  offer or file sharing is off.
  */
  std::string offerLocalFiles(const std::vector<std::string> &paths);

  //! The local clipboard no longer holds files
  void withdrawLocalFiles();

  //! True if \p fileList is clipboard data that offers files of this computer
  bool isLocalOffer(std::string_view fileList);

  //@}
  //! @name files copied on another computer
  //@{

  //! Take note of a file list received over \p channel
  /*!
  Returns false if the list is malformed or file sharing is off. The caller
  then removes the list from the clipboard it received.
  */
  bool acceptRemoteFiles(std::string_view fileList, IFileTransferChannel *channel);

  //! The interface for listing and reading the files of other computers
  /*!
  May be called from any thread. The returned object is safe to keep and use
  after this FileTransfer is destroyed.
  */
  std::shared_ptr<RemoteFiles> remoteFiles() const;

  //@}
  //! @name messages and events from a connection
  //@{

  void handleRequest(IFileTransferChannel *channel, uint32_t requestId, uint8_t kind, const std::string &arguments);
  void handleResponse(IFileTransferChannel *channel, uint32_t requestId, uint8_t mark, std::string &content);
  void handleCredit(IFileTransferChannel *channel, uint32_t requestId, uint32_t bytes);
  void handleCancel(IFileTransferChannel *channel, uint32_t requestId);

  //! Everything written to the channel has left for the network
  void channelFlushed(IFileTransferChannel *channel);

  //! The channel is gone and must not be used any more
  void channelClosed(IFileTransferChannel *channel);

  //@}

  //! Number of listings and reads that are being answered, for tests
  size_t servedRequestCount() const;

  //! How long a reader waits for the other computer before giving up, for tests
  void setReplyTimeout(int milliseconds);

  //! Encode the arguments of a request, public for tests
  static std::string
  encodeRequest(uint64_t offerId, uint32_t item, std::string_view relativePath, uint64_t offset, uint32_t credit);

private:
  struct ServedIo;
  struct IoResult;
  class Worker;

  //! A request of another computer that is being answered
  struct Served
  {
    uint8_t kind = 0;
    //! Tells this request from an earlier one with the same identifier
    uint64_t serial = 0;
    //! The open file or directory, used by the worker thread only
    std::shared_ptr<ServedIo> io;
    //! The file or directory was found and opened
    bool opened = false;
    //! The worker thread is working on this request
    bool busy = false;
    //! Everything was read, what is in \c ready is the rest
    bool finished = false;
    //! Read but not sent yet
    std::deque<std::string> ready;
    uint64_t readyBytes = 0;
    //! Bytes the other computer is ready to receive and that were not read yet
    uint64_t credit = 0;
    uint64_t sent = 0;
  };

  using ServedKey = std::pair<IFileTransferChannel *, uint32_t>;

  friend class RemoteFiles;
  friend class RemoteFileReader;
  friend struct FileTransferShared;

  void handleTask(const Event &event);
  void startRequest(
      const std::shared_ptr<FileTransferRequest> &request, uint64_t offerId, uint8_t kind, const std::string &arguments
  );
  void sendCredit(uint32_t requestId, uint32_t bytes);
  void sendCancel(uint32_t requestId);
  void finishRequest(uint32_t requestId, FileTransferStatus status);
  void updateServedCount();

  void sendError(IFileTransferChannel *channel, uint32_t requestId, Error error, const char *message);
  void pump(IFileTransferChannel *channel);
  //! Returns false when the request is finished and must be forgotten
  bool pumpServed(const ServedKey &key, Served &served);
  //! Let the worker thread do \p job for a request, jobFinished() gets the result
  void runJob(const ServedKey &key, uint64_t serial, std::function<IoResult()> job);
  void jobFinished(const ServedKey &key, uint64_t serial, IoResult &result);

  IEventQueue *m_events = nullptr;
  bool m_enabled = true;
  //! The registry is also used by the worker thread
  std::mutex m_registryMutex;
  FileOfferRegistry m_registry;

  //! Which channel leads to the computer that offers a list, oldest first
  std::vector<std::pair<uint64_t, IFileTransferChannel *>> m_remoteOffers;
  //! Which channel each of our own requests was sent on
  std::map<uint32_t, IFileTransferChannel *> m_requestChannels;
  std::map<ServedKey, Served> m_served;
  uint64_t m_nextSerial = 0;
  //! File data written to a channel since it last reported that everything left for the network
  std::map<IFileTransferChannel *, uint64_t> m_unflushed;
  //! Reads the local files, created when the first request arrives
  std::unique_ptr<Worker> m_worker;

  //! State shared with readers on other threads
  std::shared_ptr<FileTransferShared> m_shared;
  std::shared_ptr<RemoteFiles> m_remoteFiles;
  std::atomic<size_t> m_servedCount = 0;
};

} // namespace deskflow
