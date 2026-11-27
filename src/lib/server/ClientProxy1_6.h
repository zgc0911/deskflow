/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardChunk.h"
#include "deskflow/FileTransfer.h"
#include "server/ClientProxy1_5.h"

#include <string>

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5, public deskflow::IFileTransferChannel
{
public:
  ClientProxy1_6(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_6() override;

  void setClipboard(ClipboardID id, const IClipboard *clipboard) override;
  bool recvClipboard() override;
  bool parseMessage(const uint8_t *code) override;
  void setFileTransfer(deskflow::FileTransfer *fileTransfer) override;

  // IFileTransferChannel overrides
  void sendFileRequest(uint32_t requestId, uint8_t kind, const std::string &arguments) override;
  void sendFileResponse(uint32_t requestId, uint8_t mark, const std::string &content) override;
  void sendFileCredit(uint32_t requestId, uint32_t bytes) override;
  void sendFileCancel(uint32_t requestId) override;

private:
  bool recvFileRequest();
  bool recvFileResponse();
  bool recvFileCredit();
  bool recvFileCancel();

  IEventQueue *m_events;
  std::string m_clipboardDataCached;
  ClipboardChunkAssemblyState m_clipboardChunkState;
  deskflow::FileTransfer *m_fileTransfer = nullptr;
};
