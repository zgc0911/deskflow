/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_6.h"

#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "io/IStream.h"
#include "server/Server.h"

#include <cstring>

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_5(name, stream, server, events),
      m_events(events)
{
  m_events->addHandler(EventTypes::ClipboardSending, this, [this](const auto &e) {
    ClipboardChunk::send(getStream(), e.getDataObject());
  });
  // file data is sent in step with the connection draining
  m_events->addHandler(EventTypes::StreamOutputFlushed, getStream()->getEventTarget(), [this](const auto &) {
    if (m_fileTransfer != nullptr) {
      m_fileTransfer->channelFlushed(this);
    }
  });
}

ClientProxy1_6::~ClientProxy1_6()
{
  m_events->removeHandler(EventTypes::ClipboardSending, this);
  m_events->removeHandler(EventTypes::StreamOutputFlushed, getStream()->getEventTarget());
  if (m_fileTransfer != nullptr) {
    m_fileTransfer->channelClosed(this);
  }
}

void ClientProxy1_6::setFileTransfer(deskflow::FileTransfer *fileTransfer)
{
  if (m_fileTransfer != nullptr && m_fileTransfer != fileTransfer) {
    m_fileTransfer->channelClosed(this);
  }
  m_fileTransfer = fileTransfer;
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

    // a list of copied files only goes to the client if the files are on this
    // computer. files of another client would have to be passed on through
    // this computer, which is not supported.
    if (Clipboard &kept = m_clipboard[id].m_clipboard; kept.open(0)) {
      if (kept.has(IClipboard::Format::FileList) &&
          (m_fileTransfer == nullptr || !m_fileTransfer->isLocalOffer(kept.get(IClipboard::Format::FileList)))) {
        kept.remove(IClipboard::Format::FileList);
      }
      kept.close();
    }

    std::string data = m_clipboard[id].m_clipboard.marshall();

    size_t size = data.size();
    LOG_DEBUG("sending clipboard %d to \"%s\"", id, getName().c_str());

    StreamChunker::sendClipboard(data, size, id, 0, m_events, this);
  }
}

bool ClientProxy1_6::recvClipboard()
{
  // parse message
  ClipboardID id;
  uint32_t seq;

  auto r = ClipboardChunk::assemble(
      getStream(), m_clipboardDataCached, id, seq, m_clipboardChunkState, m_server->getMaximumClipboardSizeBytes()
  );

  if (r == TransferState::Started) {
    size_t size = ClipboardChunk::getExpectedSize(m_clipboardChunkState);
    LOG_DEBUG("receiving clipboard %d size=%zu", id, size);
  } else if (r == TransferState::Finished) {
    LOG(
        (CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%zu", getName().c_str(), id, seq,
         m_clipboardDataCached.size())
    );
    // save clipboard
    m_clipboard[id].m_clipboard.unmarshall(m_clipboardDataCached, 0);
    m_clipboard[id].m_sequenceNumber = seq;

    // keep the list of copied files only if the files can be asked for
    if (Clipboard &received = m_clipboard[id].m_clipboard; received.open(0)) {
      if (received.has(IClipboard::Format::FileList) &&
          (m_fileTransfer == nullptr ||
           !m_fileTransfer->acceptRemoteFiles(received.get(IClipboard::Format::FileList), this))) {
        received.remove(IClipboard::Format::FileList);
      }
      received.close();
    }
    m_clipboardDataCached.clear();
    m_clipboardDataCached.shrink_to_fit();

    // notify
    auto *info = new ClipboardInfo;
    info->m_id = id;
    info->m_sequenceNumber = seq;
    m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
  } else if (r == TransferState::Error) {
    return false;
  }

  return true;
}

bool ClientProxy1_6::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgDFileResponse, 4) == 0) {
    return recvFileResponse();
  } else if (memcmp(code, kMsgDFileRequest, 4) == 0) {
    return recvFileRequest();
  } else if (memcmp(code, kMsgDFileCredit, 4) == 0) {
    return recvFileCredit();
  } else if (memcmp(code, kMsgDFileCancel, 4) == 0) {
    return recvFileCancel();
  }
  return ClientProxy1_5::parseMessage(code);
}

void ClientProxy1_6::sendFileRequest(uint32_t requestId, uint8_t kind, const std::string &arguments)
{
  LOG_VERBOSE("send file request %u kind=%d to \"%s\"", requestId, kind, getName().c_str());
  ProtocolUtil::writef(getStream(), kMsgDFileRequest, requestId, kind, &arguments);
}

void ClientProxy1_6::sendFileResponse(uint32_t requestId, uint8_t mark, const std::string &content)
{
  LOG_VERBOSE(
      "send file response %u mark=%d size=%d to \"%s\"", requestId, mark, static_cast<int>(content.size()),
      getName().c_str()
  );
  ProtocolUtil::writef(getStream(), kMsgDFileResponse, requestId, mark, &content);
}

void ClientProxy1_6::sendFileCredit(uint32_t requestId, uint32_t bytes)
{
  LOG_VERBOSE("send file credit %u bytes=%u to \"%s\"", requestId, bytes, getName().c_str());
  ProtocolUtil::writef(getStream(), kMsgDFileCredit, requestId, bytes);
}

void ClientProxy1_6::sendFileCancel(uint32_t requestId)
{
  LOG_VERBOSE("send file cancel %u to \"%s\"", requestId, getName().c_str());
  ProtocolUtil::writef(getStream(), kMsgDFileCancel, requestId);
}

bool ClientProxy1_6::recvFileRequest()
{
  uint32_t requestId = 0;
  uint8_t kind = 0;
  std::string arguments;
  if (!ProtocolUtil::readf(getStream(), kMsgDFileRequest + 4, &requestId, &kind, &arguments)) {
    return false;
  }
  LOG_VERBOSE("received client \"%s\" file request %u kind=%d", getName().c_str(), requestId, kind);
  if (m_fileTransfer != nullptr) {
    m_fileTransfer->handleRequest(this, requestId, kind, arguments);
  }
  return true;
}

bool ClientProxy1_6::recvFileResponse()
{
  uint32_t requestId = 0;
  uint8_t mark = 0;
  std::string content;
  if (!ProtocolUtil::readf(getStream(), kMsgDFileResponse + 4, &requestId, &mark, &content)) {
    return false;
  }
  LOG_VERBOSE(
      "received client \"%s\" file response %u mark=%d size=%d", getName().c_str(), requestId, mark,
      static_cast<int>(content.size())
  );
  if (m_fileTransfer != nullptr) {
    m_fileTransfer->handleResponse(this, requestId, mark, content);
  }
  return true;
}

bool ClientProxy1_6::recvFileCredit()
{
  uint32_t requestId = 0;
  uint32_t bytes = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgDFileCredit + 4, &requestId, &bytes)) {
    return false;
  }
  LOG_VERBOSE("received client \"%s\" file credit %u bytes=%u", getName().c_str(), requestId, bytes);
  if (m_fileTransfer != nullptr) {
    m_fileTransfer->handleCredit(this, requestId, bytes);
  }
  return true;
}

bool ClientProxy1_6::recvFileCancel()
{
  uint32_t requestId = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgDFileCancel + 4, &requestId)) {
    return false;
  }
  LOG_VERBOSE("received client \"%s\" file cancel %u", getName().c_str(), requestId);
  if (m_fileTransfer != nullptr) {
    m_fileTransfer->handleCancel(this, requestId);
  }
  return true;
}
