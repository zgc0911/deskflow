# 切片 01：传输层与协议 审核报告

- 日期：2026-11-27
- 范围：剪贴板文件复制功能（提交 `80221f0f7..HEAD`）的通用传输层、协议、清单登记，以及它们接入客户端代理、服务端代理、`Computer` 的部分
- 读法：逐行读。Qt 的行为直接对照了本机 Qt 6.12.0 的私有头文件，以及从 qtbase 下载的 `v6.12.0` 标签源码（文末「依据」给出文件和行号）。没有运行测试和程序，没有改仓库。

结论先说：有 3 个高级别问题，都出在 Windows 作发送方的一侧：联接点（junction）不被识别；快捷方式（.lnk）被换成它指向的目标；核心默认以 SYSTEM 身份读文件。另有 4 个中级别、3 个低级别问题和 5 个疑点。

## 范围与读法

### 本切片文件

| 文件 | 读法 | 行号与说明 |
|---|---|---|
| `src/lib/deskflow/ClipboardFileList.h` | 完整读过 | 1-97 |
| `src/lib/deskflow/ClipboardFileList.cpp` | 完整读过 | 1-206 |
| `src/lib/deskflow/FileOfferRegistry.h` | 完整读过 | 1-108 |
| `src/lib/deskflow/FileOfferRegistry.cpp` | 完整读过 | 1-258 |
| `src/lib/deskflow/FileTransfer.h` | 完整读过 | 1-328 |
| `src/lib/deskflow/FileTransfer.cpp` | 完整读过 | 1-1127 |
| `src/lib/client/ServerProxy.cpp` | 完整读过 | 1-941，包括原有的消息循环和 `handleMessage` 每条消息回 NOOP 的逻辑 |
| `src/lib/client/ServerProxy.h` | 略读 | 只读改动块 14、19、34、56-62、65-70、89-95、132-135、163；其余是原有声明，与本功能无关 |
| `src/lib/server/ClientProxy1_6.h` | 完整读过 | 1-46 |
| `src/lib/server/ClientProxy1_6.cpp` | 完整读过 | 1-228 |
| `src/lib/deskflow/Computer.cpp` | 略读 | 读了 40-160（构造、析构）、214-222（`setClipboard`/`grabClipboard`）、404-429（`getClipboard`）；改动块是 11-12、54-55、59-60、98-99、406-428；其余键鼠代码与本功能无关 |
| `src/lib/deskflow/Computer.h` | 略读 | 改动块 16、25-26、285-290、323-325 |
| `src/lib/deskflow/IPlatformComputer.h` | 略读 | 改动块 18-24、86-115（`getClipboardFiles`、`setFileTransfer` 的约定） |
| `src/lib/deskflow/IClipboard.h` | 略读 | 改动块 48-53、60；另读 `IClipboard.cpp` 80-160（组装、`copy` 复制所有格式），并用搜索看了 62-63（解组时跳过未知格式） |
| `src/lib/deskflow/Clipboard.cpp` | 完整读过 | 1-132 |
| `src/lib/deskflow/Clipboard.h` | 略读 | 改动块 34-40、69-70 |
| `src/lib/deskflow/ProtocolTypes.h` | 略读 | 改动块 1100-1202（四条新消息的定义和文档）；另查了全部消息码，无重复 |
| `src/lib/deskflow/ProtocolTypes.cpp` | 略读 | 改动块 49-52 |
| `src/lib/client/Client.cpp` | 略读 | 读了 180-430（`setOptions`、`sendClipboard`、`leave`）、470-560（建连和断开顺序）；改动块 22、362-364、495 |
| `src/lib/server/Server.cpp` | 略读 | 读了 440-515（`switchComputer`）、1165-1232（抓取剪贴板）、1440-1487（`onClipboardChanged`）；改动块 16、274-276、1136-1138 |
| `src/lib/server/BaseClientProxy.h` | 略读 | 改动块 13-15、84-93 |
| `src/lib/base/EventTypes.h` | 略读 | 改动块 241-245 |

### 为判断行为追过去读的文件（只报原因在本切片的问题）

- `src/lib/server/ClientProxy1_0.cpp` 全文（消息循环、`disconnect` 之后的写入）；`ClientProxyUnknown.cpp` 查了按版本建代理的分支；`ClientProxy1_3/1_5` 的 `parseMessage` 链
- `src/lib/client/ServerProxy1_7.cpp` 21-30、`ServerProxy1_8.cpp` 35-69（新消息能落到基类）
- `src/lib/net/TCPSocket.cpp` 80-540、`SecureSocket.cpp` 100-240（`StreamOutputFlushed` 何时发出，TLS 下也发）；`src/lib/io/StreamFilter.cpp` 全文；`src/lib/deskflow/PacketStreamFilter.cpp` 1-140；`ProtocolUtil.cpp` 的 `readf`、`%s`、`read`
- `src/lib/deskflow/ClientApp.cpp` 135-320、`ServerApp.cpp` 140-330（`Computer` 晚于代理销毁）
- `src/lib/platform/MSWindowsComputer.cpp` 346-380、`MSWindowsClipboardFiles.cpp` 278-312（路径不设上限）、`OSXComputer.mm` 236-240 与 855-880、`OSXRemoteFileSystem.h` 30-90（并发上限）、`OSXRemoteFileSystem.cpp` 380-420 与 585
- `src/lib/platform/MSWindowsWatchdog.cpp` 100-170、`src/lib/common/Settings.cpp` 336-363、`src/apps/deskflow-daemon/DaemonApp.cpp` 80-100（核心以什么身份运行）
- `src/lib/platform/XWindowsClipboard.cpp` 415-445、`EiClipboard.cpp`、`EiClipboard.h`：格式数从 3 变 4 后 Linux 不受影响
- 测试只为核对覆盖面查看：`FileOfferRegistryTests.cpp` 40-90、`FileTransferTests.cpp` 244-375 和用例清单、`OSXClipboardFilesTests.cpp` 372-413（隐私探测）
- Qt：本机 `QtCore.framework/.../6.12.0/QtCore/private/qfilesystemmetadata_p.h`；qtbase `v6.12.0` 的 `qfilesystemengine.cpp`、`qfilesystemengine_win.cpp`、`qfilesystemiterator_win.cpp`、`qfilesystemengine_unix.cpp`、`qfileinfo.cpp`；`v6.12.1` 与 `6.12` 分支头的 `qfilesystemengine_unix.cpp`（只用于疑点 S3）

### 已核对、没有发现问题的方面

- 清单和四条消息的编解码：长度逐项检查，请求参数要求正好用完，条目名在收发两边都校验；`readf` 的字符串上限 1 MiB、包上限 4 MiB 都够用
- 老版本对端：老版本解组时跳过未知的格式号 3；请求只发给发来过清单的连接；1.5 以下的代理根本不发剪贴板
- 额度：发送方每次只读 `min(64 KiB, 剩余额度)`，没用完的退回；接收方"已缓冲 + 未确认 ≤ 窗口"的不变式成立，诚实的发送方不会被判超额；等待前先把已用的额度还回去，两边不会互等
- 锁：`FileTransferShared::mutex`、每个请求的 `mutex`、`m_registryMutex` 之间没有嵌套持有，不存在锁顺序死锁
- 生命周期：代理析构时 `channelClosed` 清掉三张表；`Computer` 在代理之后销毁；`FileTransfer` 销毁后读取方得到 `Disconnected`；过期任务按 `serial` 丢弃；请求编号要 2^32 次才回绕
- 路径：`..`、空段、开头或结尾的 `/`、Windows 的 `\` 和 `:` 都被拒；末段的尾随点和空格经 Windows 规范化后落回根本身；macOS 和 Linux 上的符号链接由 `realpath` 解开后检查
- 3 MB 剪贴板上限：清单计入剪贴板大小，发送前（`Client.cpp:400`、`Server.cpp:464`、`503`）和接收时都按同一上限检查。4096 个条目按常见名字长度约 200 KB；只有名字极长时（例如 4096 个 255 字的中文名，约 3.2 MB）才超限，这时沿用原有规则：整份剪贴板不发，写一条警告。访达复制时剪贴板上除清单外只有文件名文本（Mac 的位图转换器只读 `com.microsoft.bmp`），不会被图标数据撑大
- 服务端转发：`ClientProxy1_6::setClipboard` 只在 `isLocalOffer` 为真时保留清单，客户端 A 的清单转给 B 之前被去掉；`recvClipboard` 收下的清单交给 `acceptRemoteFiles` 登记来路，格式不对或功能关闭时去掉（例外见 S5）
- 两个剪贴板编号：`Computer::getClipboard` 对每个编号各 offer 一次；同一批文件得到同一个编号，不会来回替换；只有平台报告没有文件时才 withdraw；Windows 和 macOS 对两个编号返回同样的文件；接收方对同一清单登记两次，结果是先删后加，不会重复

## 发现

### F1 高：Windows 上的联接点（junction）不被识别，请求能读到被复制范围以外的文件

- 位置：`src/lib/deskflow/FileOfferRegistry.cpp:244-252`（`resolve`）；`src/lib/deskflow/FileTransfer.cpp:735-753`（按 `resolve` 的结果列目录、打开文件）
- 原文摘录：

```cpp
  // the canonical path has every link resolved, so this also catches a link that leads elsewhere
  const QString canonical = info.canonicalFilePath();
  if (canonical.isEmpty()) {
    return Resolved::NotFound;
  }
  if (canonical != root && !canonical.startsWith(root + QLatin1Char('/'))) {
```

- 问题：注释假定规范路径解开了所有链接，Windows 上不成立。Qt 6.12.0 的 Windows 版 `canonicalName` 返回 `slowCanonicalized(absoluteName(entry).filePath())`，后者逐段只用 `fi.isSymLink()` 判断要不要解开。`isSymLink()` 看的是 `LegacyLinkType = LinkType | AliasType | WinLnkType`；联接点因 `IO_REPARSE_TAG_MOUNT_POINT` 被标成 `JunctionType`，不在其中，所以不解开。路径中间经过联接点时，规范路径仍以根开头，检查通过；随后 `QDirIterator`/`QFile` 打开时，系统沿联接点走到别处。列目录时 `isLink()` 跳过联接点，诚实的对端看不到它；改过的对端可以直接按名字请求。卷挂载点用同一个标记，结果一样。
- 触发条件：Windows 作发送方，用户复制文件夹 `C:\proj\app`，里面有一个用 `mklink /J C:\proj\app\data D:\私人` 建的联接点（pnpm 的 `node_modules` 等工具也会建这种联接点）。改过的对端发 `DFRQ`：种类 1、条目 0、相对路径 `data`，得到 `D:\私人` 的目录内容；再发种类 2、路径 `data/任意文件`，就拿到内容。和 F3 叠加时，读取不受登录用户权限限制。另一个可能的例子是 `Documents` 里系统自带的隐藏联接点 `My Pictures` 等（指向同级的 `Pictures`），但它带"拒绝列目录"的访问控制项，实际表现未核实。
- 修复方向：不靠规范路径的字符串前缀判断范围。对根以下的每一段检查 `isSymbolicLink()`、`isJunction()`、`isShortcut()`，或检查 `FILE_ATTRIBUTE_REPARSE_POINT`，任何一段命中就拒绝。也可以打开句柄后用 `GetFinalPathNameByHandleW` 求最终路径，再和用同样方法求出的根比较。现有测试在 Windows 上跳过了所有链接用例（`QFile::link` 在 Windows 上建的是快捷方式）；建联接点不需要特权，CI 上可以补测。

### F2 高：Windows 上的快捷方式（.lnk）被换成它指向的目标：粘出来的是别的文件或整个文件夹，或者悄悄丢掉

- 位置：`src/lib/deskflow/FileOfferRegistry.cpp:77-115`（`offer`）；`FileOfferRegistry.cpp:239-252`（`resolve`）；`src/lib/deskflow/FileTransfer.cpp:902-921`（列目录）；`FileTransfer.cpp:68-71`（`isLink`）
- 原文摘录：

```cpp
    const QString canonical = info.canonicalFilePath();
    const QString name = QFileInfo(canonical).fileName();
```

- 问题：Qt 6.12.0 在 Windows 上按 `.lnk` 后缀把文件标为 `WinLnkType`，并且 `fillMetaData` 先用 `readLink`（经 `IShellLink`）把路径换成目标，再调 `GetFileAttributesEx`。所以对 .lnk 而言，`exists()`、`isFile()`、`isDir()`、`size()`、`lastModified()` 说的都是目标。目录遍历器对 .lnk 条目不预填信息，取信息时同样落到目标上。`isSymLink()` 对 .lnk 为真，`slowCanonicalized` 又把规范路径换成目标路径。本功能的 `isLink()` 是 `isSymbolicLink() || isJunction()`，不把 .lnk 当链接。结果：
  - 复制快捷方式本身：清单条目的名字、类型、大小、时间和根路径全是目标的。粘贴得到的是目标文件；指向文件夹的快捷方式，粘出整个目标文件夹。
  - 被复制的文件夹里有快捷方式：它以快捷方式的名字、目标的类型和大小被列出。读它（或列它）时 `resolve` 得到目标路径：目标在范围外，回"不是被复制的文件"，粘贴端这一项失败；目标在范围内，粘出的是目标的内容。目标已不存在的快捷方式，被当成不存在而悄悄跳过。
  - Qt 编译时没有 `fslibs` 功能时，`readLink` 返回空串，所有快捷方式都被当成不存在：复制它不进清单，文件夹里的它不被列出，都是悄悄丢掉、没有提示。官方 Windows 版 Qt 默认带这个功能，但项目用的 Qt 构建配置未核实。
  - 交付文件第 7 节 CI 记录里"快捷方式是普通文件，正常传了过去"的结论不适用于真实的快捷方式：测试建的文件叫 `way-out`，没有 `.lnk` 后缀，Qt 不把它当快捷方式。
- 触发条件：Windows 作发送方，在桌面选中"项目.lnk"（指向 `D:\项目`），按 Ctrl+C，到 Mac 的访达里粘贴：粘出来的是 `D:\项目` 整棵目录，而不是快捷方式。复制一个含程序快捷方式的文件夹（桌面、开始菜单里的文件夹）再粘贴，这些快捷方式在粘贴端读取失败。
- 修复方向：Windows 上不用 `canonicalFilePath()` 和 `QFileInfo` 的大小、类型来描述 .lnk。用不会解析快捷方式的方法取信息，例如 `GetFileAttributesExW`/`FindFirstFileW` 直接查，或用打开句柄加 `GetFinalPathNameByHandleW`（同 F1）。快捷方式按普通文件传原始字节。测试里用真实的 `.lnk` 名字。

### F3 高：Windows 上核心默认以 SYSTEM 身份运行，发送方用 SYSTEM 的权限列目录、读文件

- 位置：`src/lib/deskflow/FileTransfer.cpp:740-753`（工作线程用进程身份打开）；`src/lib/deskflow/FileOfferRegistry.cpp:76-116`（`offer` 用进程身份查看）。决定身份的代码不在本切片：`src/lib/platform/MSWindowsWatchdog.cpp:138-162`、`src/lib/common/Settings.cpp:345-346` 与 `358-361`
- 原文摘录：

```cpp
    io->file.setFileName(localPath);
    if (!io->file.open(QIODevice::ReadOnly | QIODevice::Unbuffered) ||
```

- 问题：安装版 Windows 默认是服务模式（`ProcessMode::Service`），`Daemon::Elevate` 默认为 `!isPortableMode()`，即默认开启。守护进程这时复制 `winlogon.exe` 的令牌（SYSTEM），在用户会话里启动 deskflow-core；交付文件第 4 节第 28 条也写到了"以 SYSTEM 身份运行的核心进程"。传输层读本机文件时不模拟登录用户，也不按用户权限检查。于是被复制目录里登录用户自己打不开的子目录和文件，也会被列出来、读出来、发到另一台电脑，例如别的账户的用户目录，或只对管理员和 SYSTEM 开放的目录。
- 触发条件：一台多用户的 Windows 作服务端，由管理员安装，使用默认设置。标准用户在资源管理器里复制 `C:\Users`：标准用户看得到这个文件夹和各账户的文件夹名，打不开别人的。然后在 Mac 上粘贴：core 以 SYSTEM 列出并读出 `C:\Users\<别的账户>\Documents` 等内容，粘到 Mac 上。
- 修复方向：工作线程访问文件前模拟会话里的登录用户，用完立即还原，例如 `WTSQueryUserToken` 加 `ImpersonateLoggedOnUser`，结束时 `RevertToSelf`。或者打开、列目录之前用 `AccessCheck` 按登录用户的令牌校验。`offer()` 的查看也按用户身份做。

### F4 中：工作线程持有 `m_registryMutex` 时做文件系统调用，核心线程跟着卡住

- 位置：`src/lib/deskflow/FileTransfer.cpp:719-722`；`src/lib/deskflow/FileOfferRegistry.cpp:239-246`。核心线程上取同一把锁的地方：`FileTransfer.cpp:543`（`setEnabled`）、`596`（`offerLocalFiles`）、`612`（`withdrawLocalFiles`）、`622`（`isLocalOffer`）；调用方是 `Computer.cpp:419-423` 和 `ClientProxy1_6.cpp:66-68`
- 原文摘录：

```cpp
      std::scoped_lock lock{m_registryMutex};
      resolved = m_registry.resolve(offerId, item, relativePath, localPath);
```

- 问题：设计第 10 条把找文件、打开文件放到工作线程，就是为了不让慢盘、网络盘拖住核心线程。但 `resolve()` 里的 `exists()` 和 `canonicalFilePath()` 是持锁做的。Windows 上后者对路径的每一级各查一次文件系统，遇到 .lnk 还要经 COM 读快捷方式。核心线程上有几处本身不碰磁盘的操作也要这把锁：本机剪贴板不再有文件时的 `withdrawLocalFiles`，以及每次把剪贴板发给客户端时的 `isLocalOffer`。工作线程在文件系统里卡多久，它们就等多久，键鼠也停多久。
- 触发条件：Windows 服务端复制了网络共享（或已休眠的移动硬盘）上的文件夹，Mac 正在粘贴。中途共享连不上（VPN 断开、NAS 休眠），工作线程下一次 `resolve()` 卡在文件系统查询里几十秒。这时出现下面任一情况，核心线程都会等锁，光标停住，直到那次查询超时返回：
  - 用户在 Windows 上复制一段文字后把光标移向客户端：`switchComputer` → `getClipboard` → `withdrawLocalFiles`
  - 光标进入一个剪贴板待更新的客户端：`setClipboard` → `isLocalOffer`
- 修复方向：持锁只取出根路径（复制一份 `QString`），在锁外做存在性、规范化和范围检查；必要时事后再持锁确认清单仍然有效。

### F5 中：发送方严格按请求编号先后发送，进行中的大文件会饿死同一连接上后来的请求，后者 30 秒后超时失败

- 位置：`src/lib/deskflow/FileTransfer.cpp:825-842`（`pump`）、`852-860`（`pumpServed` 的发送循环）；接收方的超时：`FileTransfer.cpp:126`、`247-255`、`333-340`
- 原文摘录：

```cpp
  uint64_t &unflushed = m_unflushed[channel];
  while (!served.ready.empty() && unflushed < kMaxUnflushedBytes) {
```

- 问题：`m_served` 是按（连接, 请求编号）排序的 `std::map`。`pump` 每次从编号最小的请求开始，同一连接上的所有请求共用一份 128 KiB 的"未清空"额度。下面这一段是推断，未实测：工作线程读本地盘比网络快，网络受限时，每次"输出已清空"那一刻，编号最小的读请求手里总有 128 KiB 预读好的数据，一次就把额度用完；编号大的请求（后发起的读、列目录）数据早已读好，却一直发不出去。只要前一个请求还有额度（接收方边用边放行），后一个就一个字节也收不到。接收方的 `RemoteFileReader::read` 和 `RemoteFiles::listDirectory` 30 秒没收到任何消息就判超时，放弃请求。
- 触发条件：Mac 在访达里粘贴 Windows 上复制的一个 2 GB 视频，走 Wi-Fi（约 25 MB/s，要 80 秒）。拷贝进行中，用户在 Windows 上又复制了一个文件夹，在 Mac 的另一个访达窗口粘贴。新请求的编号都更大，30 秒内收不到数据，得到 `TimedOut`，第二次粘贴失败。现有测试一次只取一个文件（`Fetch::all` 顺序执行），覆盖不到。
- 修复方向：轮转发送：每次 `pump` 从上次停下的下一个请求开始，或者每个请求每轮最多发一块。

### F6 中：每次读剪贴板，核心线程都把所有被复制的路径逐个查看一遍；超过 4096 个也要先全部查完

- 位置：`src/lib/deskflow/FileOfferRegistry.cpp:76-125`；`src/lib/deskflow/Computer.cpp:419-420`；调用频率见 `src/lib/server/Server.cpp:469-475`
- 原文摘录：

```cpp
  if (list.entries.size() > ClipboardFileList::s_maxEntries) {
```

- 问题：`offer()` 对每个路径都做链接检查、存在检查、`canonicalFilePath()`、取大小和时间，全在核心线程上持锁完成。"超过 4096 个"的判断在循环之后，超限时前面的查看全部白做。服务端每次光标离开主屏、而主屏拥有剪贴板时，都对两个剪贴板编号各调一次 `getClipboard`（`Server.cpp:469-475`）：每越过一次屏幕边缘，整批文件就被查看两遍。Windows 上 `canonicalFilePath()` 对路径的每一级各查一次文件系统，总代价约等于"文件数 × 路径深度"；在网络共享上，每次查询还要一次网络往返。`IPlatformComputer::getClipboardFiles` 的约定写明"Must not look at files on a volume that may not answer"，而 `offer()` 拿到路径后立即在核心线程上查看了它们。
- 触发条件：Windows 服务端在资源管理器里全选一个有 2 万张照片的文件夹并复制，然后把光标移到 Mac。每次越界，核心线程都要把 2 万个路径各查看两遍，每个路径若干次文件系统调用。按每次调用十几到几十微秒估算，每次越界停顿数秒（未实测），而结果因超过 4096 被丢弃。复制网络共享上的几十个文件时，每次越界也要付出"文件数 × 深度 × 往返时间"。
- 修复方向：路径数超过上限时直接放弃，不再查看；同一批路径缓存上次的结果，剪贴板没变就不重查；或者把查看挪到工作线程，清单准备好后再补发剪贴板。

### F7 中：读不了的目录被当成空目录回答，粘贴端悄悄得到空文件夹

- 位置：`src/lib/deskflow/FileTransfer.cpp:740-743`、`902-925`
- 原文摘录：

```cpp
      io->listing = std::make_unique<QDirIterator>(
          localPath, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot
      );
```

- 问题：`QDirIterator` 没有错误接口，打不开目录或中途出错时只是没有（更多）条目。发送方据此回"结束"（`result.end = !io->listing->hasNext();`），接收方得到一个成功的空列表或截断的列表，当成完整内容。读文件失败会回 `ReadFailed`，读目录失败却没有任何提示。
- 触发条件：Mac 作发送方，被复制的文件夹里有一个没有读权限的子文件夹（例如权限为 `000`，或属于别的用户）。查看和规范化都只需要上级目录的搜索权限，所以它照常被列为文件夹。到 Windows 粘贴时，列这个子文件夹的 `opendir` 失败，回空列表，资源管理器建出一个空文件夹，不报错。网络盘在列目录中途掉线时，列表被截断，后面的文件悄悄缺失。macOS 隐私保护拒绝访问（用户在授权框里点了"不允许"）时 `opendir` 大概也会失败，但这一点未实测：交付文件里的探测只测了读文件。
- 修复方向：列目录前先用平台调用自己打开一次目录（POSIX `opendir`、Windows `FindFirstFileExW`），失败就回 `ReadFailed`；遍历中途出错也回错误，而不是"结束"。

### F8 低：本机剪贴板被另一台电脑的内容接管后，本机的文件清单不作废

- 位置：`src/lib/deskflow/Computer.cpp:214-222`
- 原文摘录：

```cpp
void Computer::grabClipboard(ClipboardID id)
{
  m_computer->setClipboard(id, nullptr);
}
```

- 问题：只有 `getClipboard` 会 offer 或 withdraw。另一台电脑复制了东西、本机被 `grabClipboard`/`setClipboard` 接管之后，系统剪贴板上已经不是本机复制的文件了，但登记表里的当前清单还是旧的。它不进入设计第 7 条"被替换后 60 秒内无请求就作废"的流程，可以一直被读下去，直到本机下次自己复制东西。
- 触发条件：Windows 服务端复制文件夹 X，清单已发给 Mac；随后用户在 Mac 上复制一段文字，服务端的剪贴板被换成这段文字。几小时后，Mac 虚拟磁盘上仍保留着 X 的目录（最多保留 4 份清单），读它照样能拿到 X 的内容；改过的客户端也能一直读。
- 修复方向：在 `Computer::setClipboard` 和 `grabClipboard` 里调用 `withdrawLocalFiles()`。清单转入 60 秒保留期，正在进行的粘贴不受影响。

### F9 低：对端发来的空数据消息不受额度限制，接收方无限积压

- 位置：`src/lib/deskflow/FileTransfer.cpp:1077-1083`；读取方只在 `buffered > 0` 时消费，见 `FileTransfer.cpp:247`、`258-271`
- 原文摘录：

```cpp
      if (request->buffered + content.size() > limit) {
        tooMuch = true;
      } else {
        request->buffered += content.size();
        request->chunks.push_back(std::move(content));
        ++request->activity;
      }
```

- 问题：空内容通过额度检查后被加进 `chunks`，`buffered` 不变；读取方只在 `buffered > 0` 时才取 `chunks`。每条空消息都留下一个节点，额度挡不住。同时 `activity` 增加，读取方的超时也不会触发。
- 触发条件：改过的对端针对一个进行中的读请求，不停发送内容为空的 `DFRS`（标记 1）。每条在线上 17 字节，接收方就多一个 `std::string` 节点，内存一直涨。
- 修复方向：空数据块直接丢弃，不加入、不计活动；或者按协议错误取消请求。

### F10 低：销毁 `FileTransfer` 时无条件等工作线程，工作线程卡住时核心退不出去

- 位置：`src/lib/deskflow/FileTransfer.cpp:437-445`、`497-498`
- 原文摘录：

```cpp
  ~Worker()
  {
    {
      std::scoped_lock lock{m_mutex};
      m_stop = true;
    }
    m_changed.notify_all();
    m_thread.join();
  }
```

- 问题：正在执行的任务（打开、读取、列目录、`resolve`）无法打断；`~FileTransfer` 在核心线程上 `join`，任务卡多久，退出就卡多久。
- 触发条件：Mac 作发送方，Windows 粘贴 Mac 桌面上的文件，Mac 弹出"访问桌面文件夹"的授权框，工作线程阻塞在 `open` 里等人点（交付文件第 7 节的探测确认读桌面文件会弹框并等待）。这时用户退出 Deskflow，或改设置让界面重启核心：core 停在 `~FileTransfer` 里，直到有人点那个框。网络盘失联时同理，要等系统超时。
- 修复方向：退出时对卡住的任务设上限，例如等一段时间后不再 `join`，让工作线程只持有共享状态、自行结束；或者把可能阻塞的调用放进可以放弃的子任务里。

## 疑点

### S1 "忙"被当成失败；每连接 16 个请求的上限和 Mac 端只差 1

- 位置：`src/lib/deskflow/FileTransfer.cpp:33`、`682-686`、`73-86`；Mac 端见 `src/lib/platform/OSXRemoteFileSystem.h` 的 `streams = 12`、`listings = 3`，和 `OSXRemoteFileSystem.cpp:585`
- 说明：`Busy`（错误码 7）经 `statusFromError` 变成 `Failed`，粘贴端分不出"稍后再试"和真失败。Mac 端最多 12 个下载加 3 个列目录，共 15 个。`stream->reader = files->openFile(...)` 先建新读取器（先发新请求），再销毁旧的（后发取消），服务端会短暂多算一个。几个下载同时更换时可能超过 16，其中一个直接失败。需要切片 02 确认 Mac 端的实际并发。

### S2 Windows 保留设备名没有被拒绝

- 位置：`src/lib/deskflow/FileOfferRegistry.cpp:195-200`
- 说明：`CON`、`NUL`、`AUX`、`COM1` 这类名字能通过 `isValidPathComponent`。Windows 10 及更早的路径规则会把末段是设备名的路径映射到设备。如果 `QFileInfo::exists()`/`isFile()` 对这类路径为真，改过的对端请求 `COM1` 可能让工作线程阻塞在串口读取上：所有文件传输停住，退出时卡在 `join`（见 F10）。没有在 Windows 上验证 Qt 对这类路径的返回值。

### S3 将来的 Qt 版本里，`canonicalFilePath()` 在 macOS 上会打开文件

- 位置：`src/lib/deskflow/FileOfferRegistry.cpp:87`、`245`
- 说明：Qt 6.12.0 和 6.12.1 在 macOS 上的 `canonicalName` 只调 `realpath()`，和交付文件第 7 节隐私探测的结果一致（不弹框）。qtbase `6.12` 分支头（尚未发布）已改为先 `QT_OPEN(entry.nativeFilePath(), O_RDONLY | O_PATH | O_NONBLOCK)`，再用 `F_GETPATH`。macOS 的 SDK 没有 `O_PATH`，Qt 把它定义为 0，等于以只读方式打开文件。换到这样的 Qt 后，`offer()` 在核心线程上对桌面、文稿里的文件调用 `canonicalFilePath()`，就可能弹授权框并阻塞核心线程；Mac 被 Deskflow 远程操控时，用户点不到这个框。F1、F2 的修复（不依赖 `canonicalFilePath()`）也能消除这个风险。

### S4 发送节奏只限住了 Deskflow 自己的输出缓冲

- 位置：`src/lib/deskflow/FileTransfer.cpp:38-39`、`818-823`、`852-860`
- 说明："输出已清空"事件在数据进入内核发送缓冲时就发出，内核缓冲里排着的文件数据仍在键鼠消息前面。键鼠走服务端到客户端的方向。Windows 作服务端时，内核发送缓冲按带宽和往返时间自动调整，排队时间大约一个往返，影响应当很小。没有实测；交付文件第 7 节的测量用的是内存连接，测不到这一层。

### S5 改过的客户端可以冒用服务端自己的清单编号

- 位置：`src/lib/server/ClientProxy1_6.cpp:66-69`、`106-110`；`src/lib/deskflow/FileTransfer.cpp:616-624`
- 说明：客户端 B 收到过服务端的清单 X，可以发一份编号也是 X、名字和类型由它编的"清单"。服务端照收；转发给客户端 C 时，`isLocalOffer` 只比较编号，所以不去掉。C 看到的是 B 编的名字和类型，取到的却是服务端自己的 X。没有新的泄露，影响是名字和类型可被伪造。

## 依据

Qt 源码是从 qtbase 的 GitHub 镜像下载原文件后逐行查看的。

- 本机 Qt 6.12.0 私有头 `qfilesystemmetadata_p.h`：第 87 行 `LegacyLinkType = LinkType | AliasType | WinLnkType`；第 79-80 行 `JunctionType`、`WinLnkType` 只在 Windows 上非零；`fillFromFindData` 把 `IO_REPARSE_TAG_SYMLINK` 标为 `LinkType`，把目录上的 `IO_REPARSE_TAG_MOUNT_POINT` 标为 `JunctionType`
- `v6.12.0` `qfileinfo.cpp`：1165-1170 行 `isSymLink()` 用 `isLegacyLink()`；1194-1199 行 `isSymbolicLink()` 用 `isLink()`；1271-1276 行 `isShortcut()` 用 `isLnkFile()`；1315-1320 行 `isJunction()` 用 `isJunction()`
- `v6.12.0` `qfilesystemengine.cpp`：49-98 行 `slowCanonicalized`，第 79 行 `if (fi.isSymLink()) {`，没有处理联接点
- `v6.12.0` `qfilesystemengine_win.cpp`：946-947 行 `canonicalName` 返回 `slowCanonicalized(absoluteName(entry).filePath())`；1394-1396 行按 `.lnk` 后缀设 `WinLnkType` 并 `fname = QFileSystemEntry(readLink(entry));`，1400-1415 行随后对 `fname`（目标）调 `GetFileAttributesEx`，`fname` 为空时返回"不存在"；910-911 行 `getRawLinkPath` 对 .lnk 调 `readLink`；699-733 行 `readLink` 用 `IShellLink`，没有 `fslibs` 时返回空串
- `v6.12.0` `qfilesystemiterator_win.cpp`：110-111 行，只对目录和非 `.lnk` 条目调 `fillFromFindData`
- `v6.12.0` `qfilesystemengine_unix.cpp`：719-740 行，Darwin 上 `canonicalName` 只调 `realpath()`，失败时返回空。`v6.12.1` 相同；`6.12` 分支头 95 行 `#  define O_PATH 0`、721-737 行在 `F_GETPATH` 可用时先 `QT_OPEN(..., O_RDONLY | O_PATH | O_NONBLOCK)`
- 默认身份：`Settings.cpp:345-346`（`Daemon::Elevate` 默认 `!isPortableMode()`）、`358-361`（Windows 安装版默认 `ProcessMode::Service`）、`MSWindowsWatchdog.cpp:138-162`（复制 `winlogon.exe` 的令牌）
