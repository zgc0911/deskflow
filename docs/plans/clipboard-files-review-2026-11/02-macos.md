# 审核报告：切片 02 macOS 端（NFS 虚拟磁盘与剪贴板）

- 日期：2026-11-27
- 范围：提交 `80221f0f7..HEAD` 里 macOS 端的虚拟磁盘、NFS 服务、剪贴板读写，以及 `OSXComputer` 的改动部分。
- 做法：只读审核。没有改仓库里的任何文件，没有运行测试和程序。
- 结果：发现 4 条，其中高 1 条（F1：挂载期间本机其他用户能拿到磁盘的根句柄），低 3 条（F2–F4）；疑点 6 条（S1–S6）。

## 范围与读法

### 本切片的文件

| 文件 | 读法 | 说明 |
|---|---|---|
| `src/lib/platform/OSXNfsServer.h` | 完整读过（1–163） | |
| `src/lib/platform/OSXNfsServer.cpp` | 完整读过（1–887） | 每个过程的应答布局都对照了 RFC 1813 和 RFC 5531 |
| `src/lib/platform/OSXRemoteFileSystem.h` | 完整读过（1–137） | |
| `src/lib/platform/OSXRemoteFileSystem.cpp` | 完整读过（1–1083） | |
| `src/lib/platform/OSXRemoteFileVolume.h` | 完整读过（1–154） | |
| `src/lib/platform/OSXRemoteFileVolume.cpp` | 完整读过（1–533） | |
| `src/lib/platform/OSXPasteboardFiles.h` | 完整读过（1–52） | |
| `src/lib/platform/OSXPasteboardFiles.mm` | 完整读过（1–123） | |
| `src/lib/platform/OSXClipboardFiles.h` | 完整读过（1–66） | |
| `src/lib/platform/OSXClipboardFiles.cpp` | 完整读过（1–99） | |
| `src/lib/platform/OSXComputer.h` | 略读：只看了本特性的改动（`git diff 80221f0f7`） | 其余部分本特性没改，不在切片内 |
| `src/lib/platform/OSXComputer.mm` | 略读：改动部分，以及 236–240（getClipboard）、680–720（剪贴板定时器）、820–910（setClipboard、setClipboardFiles、getClipboardFiles、setFileTransfer、checkClipboards） | 同上 |

### 为判断行为跟进去读的依赖（不在本切片，不审）

| 文件 | 读了哪些 | 为了什么 |
|---|---|---|
| `src/lib/deskflow/FileTransfer.h` | 全文 | RemoteFiles、RemoteFileReader 的阻塞和超时 |
| `src/lib/deskflow/FileTransfer.cpp` | 33、40–400、495–520、680–780、810–835、977–1125 | 错误码映射、读者超时与放行额度、每条连接 16 个请求的上限、发送端打开文件和 seek、取消、发请求、收应答、关连接和析构时怎样结束请求 |
| `src/lib/deskflow/Computer.cpp` | 全文 | getClipboard 与两个剪贴板编号 |
| `src/lib/client/Client.cpp` | 225–270、375–420、645–675 | leave、setClipboard、sendClipboard、handleClipboardGrabbed |
| `src/lib/client/ServerProxy.cpp` | 只看改动 | acceptFileList 和 setClipboard 的先后 |
| `src/lib/server/Server.cpp` | 490–512、1470–1490 | 服务端什么时候把剪贴板发给客户端 |
| `src/lib/deskflow/ClientApp.cpp` | 130–150、265–375 | Computer 什么时候建、什么时候删 |
| `src/lib/platform/OSXClipboard.cpp` | 1–200 | 旧剪贴板接口 synchronize 的语义 |
| `src/lib/deskflow/IClipboard.cpp` | 121–167 | copy() 遍历哪些格式 |
| `src/lib/deskflow/IClipboard.h` | 只看 grep 命中的 32–61 行附近 | 格式枚举里有 FileList |
| `src/unittests/platform/OSXClipboardFilesTests.cpp` | 264–322；另有 54、65 两行是 grep 命中 | put 不等磁盘的测试用了哪些条目 |
| `src/unittests/platform/OSXRemoteFileVolumeTests.cpp` | 535–580 | 访问控制测试 |
| `src/lib/platform/CMakeLists.txt` | 只看改动 | 新文件只在 APPLE 分支编译 |
| `docs/plans/交付-剪贴板文件复制.md` | 第 4–7 节 | 设计意图 |
| `docs/dev/protocol_reference.md` | "Sharing Files Through the Clipboard" 一节 | 协议约定 |

### 执行过的命令

都是只读命令：`git log`、`git diff`、`grep`、`sed -n`、`wc`、`ls`、`ps -axo user,pid,args`、`/usr/bin/time -p ps -axo args`、`netstat -an -p tcp`、`mount`。另外用 `mkdir -p` 在会话临时目录里建了放本报告的目录。

### 核对过、未发现问题的部分

- ONC RPC：
  - 调用头、凭证和验证器的解析都对。
  - 接受后的应答带 AUTH_NONE 验证器；PROG_UNAVAIL、PROG_MISMATCH（带最低和最高版本）、PROC_UNAVAIL、GARBAGE_ARGS 的写法都对。
  - 记录标记：多片段能重组，最后一片的标志位处理正确，有 256 KB 上限。
- MOUNT v3：NULL、MNT（状态、句柄、认证方式数组）、DUMP 和 EXPORT（回空链表）、UMNT、UMNTALL 都对。
- NFS v3 各过程的成功和失败应答都对：
  - fattr3 的字段顺序和宽度、post_op_attr、wcc_data。写类过程一律回 ROFS。
  - LOOKUP、ACCESS、READLINK、READ（count、eof、数据）。
  - READDIR 和 READDIRPLUS：cookie 从 3 开始编号，"." 是 1，".." 是 2，续读时的位置换算正确；READDIRPLUS 的条目带属性和句柄。
  - FSSTAT、FSINFO（单次最多读 1 MB，属性位 FSF3_HOMOGENEOUS）、PATHCONF。
  - 长度不对的句柄回 BADHANDLE。
- 读文件：
  - 读请求从不回"0 字节但没到文件尾"。
  - 文件变短时，用下载到的实际末尾修正大小，READ 应答里的属性随之更新。
  - 读到末尾之后，回文件尾标记。
- 下载流里的数据块始终从 begin 到 end 连续。在选中一条流和拷出数据之间，`advanceServed` 的强制前移可能裁掉数据，核过后确认不会裁到当前要读的位置：
  - 已发出的区间都不超过 servedTo + span，所以强制前移之后 servedTo 小于"当前偏移 + count"。
  - keepBehind（2 MB）不小于单次读的上限（1 MB），所以被裁掉的数据都在当前偏移之前。
  - 这只在默认参数下成立：把 keepBehind 调到小于 1 MB 就不成立了。
- 锁：
  - 持有文件系统状态锁时，只会调用 `RemoteFileReader::cancel` 和 `RemoteFiles::openFile`，两者都只往事件队列里放事件，不阻塞。
  - 事件线程上的 `startRequest`、`finishRequest`、`handleResponse`、`sendCancel`、`sendCredit` 都已读过，只用传输层自己的锁，不回调 `OSXRemoteFileSystem`，所以不会形成锁环。
  - 服务端的 `m_mutex` 和磁盘的 `m_mutex`，持有期间都不调用文件系统。
- 唤醒：列目录、下载、读请求的等待条件都在锁内修改、锁内检查，没发现丢失唤醒。下载线程的等待带 500 毫秒超时。
- 服务端停止：先唤醒正在等的调用，再关连接，然后等所有线程计数归零。线程在锁内发通知，之后不再碰服务对象。
- 发送端每条连接最多同时处理 16 个请求：
  - Mac 端最多同时开 12 条下载流和 3 个列目录，共 15 个。
  - 停掉一条流时，取消消息是在状态锁内、先于新请求排进事件队列的；发送端的 `handleCancel` 收到后立即删掉这个请求。
  - 所以不会撞上"并行请求太多"。
- 挂载与清理：
  - 卸载用的是系统认的路径，经本地目录 realpath 求出。
  - 清理遗留时只用 rmdir，不递归删除。
  - mount_nfs 以 `POSIX_SPAWN_CLOEXEC_DEFAULT` 启动，不会继承服务端的套接字。
- 两个剪贴板编号：
  - 第二次 put 同样的路径、剪贴板计数也没变时，不再写一遍。
  - 写完立即 synchronize 并记下计数，checkClipboards 据此不把自己的写入当成用户复制。
- 新文件只在 CMake 的 APPLE 分支里加入构建，不影响 Linux。

## 发现

### F1 高：挂载期间本机其他用户能拿到根句柄，读走对端复制的所有文件

- 位置：
  - `src/lib/platform/OSXNfsServer.cpp:598`（MNT 的放行条件）、`:604`、`:545`
  - `src/lib/platform/OSXRemoteFileVolume.cpp:430`、`:441`、`:459`、`:431`
  - `src/lib/platform/OSXRemoteFileSystem.cpp:827`
- 原文摘录：

  `src/lib/platform/OSXNfsServer.cpp:598`、`:604`、`:545`：

  ```cpp
      if (!m_mountAllowed || path != m_exportPath) {
  ```

  ```cpp
      result.handle(m_fileSystem.rootHandle());
  ```

  ```cpp
    // who calls: not checked, nothing but this computer's own client can connect and find a handle
  ```

  `src/lib/platform/OSXRemoteFileVolume.cpp:430`、`:441`、`:459`、`:431`：

  ```cpp
    const std::string exportPath = "/" + randomHex(16);
  ```

  ```cpp
        {m_options.mountCommand, "-o", options, "127.0.0.1:" + exportPath, point}, m_options.mountTimeoutMs, m_cancel,
  ```

  ```cpp
    server->setMountAllowed(false);
  ```

  ```cpp
    auto server = std::make_unique<nfs::Server>(*m_fileSystem);
  ```

  `src/lib/platform/OSXRemoteFileSystem.cpp:827`（在构造函数里）：

  ```cpp
    arc4random_buf(root.handle.data(), root.handle.size());
  ```

- 问题：交付文件第 7 节「虚拟磁盘的访问控制」写的是"挂载请求只接受第一次，也就是我们自己发起的那次，之后一律拒绝"。实现和这句话不一致：
  1. 在 `setMountAllowed(false)` 之前，服务端对每一个带正确导出路径的 MNT 请求都回根句柄，次数不限。`setMountAllowed(false)` 要等 mount_nfs 退出、`isMountedAt` 核实之后才调用。
  2. 导出路径并不保密，有两条途径能看到：
     - 命令行参数（已在本机核实）。导出路径和端口都是 mount_nfs 的命令行参数。`/bin/ps` 是 setuid root 的程序（`-rwsr-xr-x root wheel`），普通用户运行它能看到其他用户进程的完整参数：本机 `ps -axo user,pid,args` 列出了 `_windowserver`、`_usbmuxd` 等其他用户进程的参数。这条途径在 mount_nfs 运行期间有效。
     - 挂载列表（本机没有观察到）。`mount` 和 `getfsstat` 向任何用户列出全部挂载及其来源：本机 `mount` 能列出系统卷的来源，但当时没有 NFS 挂载，没看到 `127.0.0.1:/<导出路径>` 这一行。这条途径从 mount(2) 返回起，到 `setMountAllowed(false)` 为止，持续时间没有测。
  3. 服务端不检查 AUTH_SYS 里的身份（第 545 行的注释）。
  4. 根句柄在 `OSXRemoteFileSystem` 构造时生成一次。重挂时只换服务、端口和导出路径（`OSXRemoteFileVolume.cpp:431` 用的是同一个文件系统对象），根句柄和其他句柄都不变。所以根句柄一旦被拿到，在核心进程整个生命周期里都能用。重挂后的新端口，从 `netstat -an` 列出的监听端口或重挂时 mount_nfs 的参数里就能找到（本机 `netstat -an -p tcp` 能列出 127.0.0.1 上的监听端口）。
  - 测试 `OSXRemoteFileVolumeTests::otherClients_areTurnedAway` 第 565–574 行验证的就是这个行为：还没挂载时，知道路径的别的客户端发 MNT 能拿到句柄。
- 触发条件：
  1. 同一台 Mac 上有另一个账户，例如快速用户切换后在后台登录的账户。
  2. 这个账户同时运行几个错开的 `/bin/ps -axo args` 循环，一旦看到 `/sbin/mount_nfs`，就取出 `port=P` 和 `127.0.0.1:/<hex>`。本机跑一次 `ps` 约 20 毫秒，几个循环错开后，几毫秒就能看一次。整次挂载约 21 毫秒（交付文件实测），能不能看到 mount_nfs 是概率性的。
  3. 一看到就连上 127.0.0.1:P，发 MOUNTPROC3_MNT(`/<hex>`)。
  4. 拿到根句柄后，用 READDIRPLUS 逐级拿到每个清单目录和条目的句柄，再发 READ，就能读出 Windows 上复制过的任何文件和文件夹。内容由本机按需向对端拉取。
  - 只需成功一次：句柄在核心进程的整个生命周期内有效，而且核心进程每次启动、每次重挂都会再给一次机会。
- 修复方向：
  1. MNT 只回答一次：路径比对通过后用 `m_mountAllowed.exchange(false)`，之后的请求一律拒绝。
  2. 每次挂载换一个新的根句柄，必要时连同全部句柄一起换。记下答出根句柄的次数，mount_nfs 返回后如果不是恰好一次，就丢弃这次挂载，换句柄重挂。
  3. 从根上说，别让秘密出现在命令行上。可以直接调用 mount(2) 并带上根句柄，不走 MNT；也可以核对 MNT 所在连接的对端端口是不是刚启动的 mount_nfs 子进程的套接字（用 `proc_pidinfo` 查）。
  4. 可以再加一条 AUTH_SYS 的 uid 必须等于 `getuid()` 的检查作补充。uid 能伪造，这条只能挡住直接用 mount_nfs 的人。

### F2 低：新清单挤掉最早的清单时，不管它是否正在被粘贴

- 位置：`src/lib/platform/OSXRemoteFileSystem.cpp:881`（循环从这里开始，删除在 `:888`）
- 原文摘录：

  `src/lib/platform/OSXRemoteFileSystem.cpp:881`、`:888`：

  ```cpp
    while (state.offers.size() > state.limits.offers) {
  ```

  ```cpp
      state.remove(oldest);
  ```

- 问题：
  - `remove` 会删掉整棵树和所有句柄，并停掉它的下载。正在等数据的读请求醒来后 `find(id)` 找不到节点，回 Stale，访达里进行中的拷贝报错中断。
  - 代码里已有判断"是否正在被读"的 `isBeingRead()`，这里没有用。
  - 发送端（交付文件第 4 节第 7 条）特意让还有请求的旧清单继续有效，接收端这里却直接删掉。
- 触发条件：
  1. 在 Mac 上粘贴一个要拷几分钟的大文件夹。
  2. 拷贝期间，用户在 Windows 上先后复制 4 次不同的文件，每次都把光标移进 Mac。服务端在切换到 Mac 时把剪贴板发过去（`Server.cpp:497–506`），每次复制产生一份新清单。
  3. 第 4 份清单到达时，正在粘贴的那份被删，访达的拷贝失败。
- 修复方向：超出上限时，跳过 `isBeingRead()` 为真或最近 freshMs 内还有调用的清单，等它闲下来再删。上限可以暂时超出一两份。

### F3 低：目录分页用"位置"当 cookie、验证码恒为 0，但重新列目录会挪动位置

- 位置：
  - `src/lib/platform/OSXNfsServer.cpp:804`（cookie 编号规则在 770–805）
  - `src/lib/platform/OSXRemoteFileSystem.cpp:334–336`（`setEntries`，331–387）
  - 接口约定：`src/lib/platform/OSXNfsServer.h:80`
- 原文摘录：

  `src/lib/platform/OSXNfsServer.cpp:803–804`：

  ```cpp
      // the listing is always the same, so there is nothing to verify
      const uint8_t verifier[8] = {};
  ```

  `src/lib/platform/OSXRemoteFileSystem.cpp:334–336`：

  ```cpp
      std::map<std::string, uint64_t> before = std::move(directory.byName);
      directory.byName.clear();
      directory.children.clear();
  ```

  `src/lib/platform/OSXNfsServer.h:80`：

  ```cpp
    them. The order of a directory never changes.
  ```

- 问题：
  - 停顿超过 freshMs（3 秒）之后的下一次调用，会开始新的一次查看。这时目录会重新向对端列一遍，`setEntries` 按新列表重建 `children`。
  - 对端如果在两次之间删掉或新增了一个条目，它后面所有条目的位置都会变。客户端拿旧 cookie 读下一页时，会漏掉或重复条目。
  - 验证码恒为 0，服务端也不看客户端带来的验证码，所以客户端发现不了。RFC 1813 的做法是：验证码变了就回 NFS3ERR_BAD_COOKIE。
- 触发条件：
  1. 粘贴一个超过一页的文件夹。一页多大由客户端请求里的 maxcount 决定（服务端的预算是 maxcount 减 256 字节）；macOS 客户端实际给多大，没有核实。
  2. 访达读完第一页之后停顿 3 秒以上，例如弹出"替换还是跳过"的对话框等用户点。
  3. 这期间，Windows 上这个文件夹里排在前面的一个文件被删。
  4. 访达继续用旧 cookie 读第二页，第二页原来的第一个条目被跳过。粘贴出来的文件夹少一个文件，而且不报错。
  - 发生的可能性低。
- 修复方向：
  - 每个目录记一个列表版本号，重建 `children` 时加一，并写进 cookie 验证码；收到 cookie 非 0、验证码却是旧的请求时，回 NFS3ERR_BAD_COOKIE。
  - 或者重建时保留旧条目的位置：被删的位置留空、读的时候跳过，新增的条目追加在末尾。

### F4 低：连接不限数量、不会超时断开，接受连接的线程遇到暂时性错误就永久退出

- 位置：`src/lib/platform/OSXNfsServer.cpp:445`（错误处理在 443–449）；另见 `:456–470`、`:510`
- 原文摘录：

  `src/lib/platform/OSXNfsServer.cpp:443–449`：

  ```cpp
      const int socket = ::accept(m_listenSocket, nullptr, nullptr);
      if (socket < 0) {
        if (errno == EINTR || errno == ECONNABORTED) {
          continue;
        }
        break;
      }
  ```

- 问题：
  - 每个连接占核心进程一个文件描述符和一条线程。数量没有上限，空闲连接也不会被断开：`receiveLoop` 在 `recv` 里一直等下去。
  - EMFILE、ENFILE、ENOBUFS 这类暂时性错误也会让 `acceptLoop` 结束。之后这台服务再也不接受新连接，但磁盘的状态仍是 Mounted。只有等系统把磁盘判死并卸掉（约一分钟），5 秒一次的检查才会发现并重挂。
  - `std::thread` 构造失败时会抛异常。`acceptLoop`（`:463`）和 `receiveLoop`（`:510`）都没有接住，会导致 `std::terminate`。
- 触发条件：
  1. 本机另一个用户反复连接 127.0.0.1 上的这个端口，连上后不关。
  2. 核心进程的文件描述符被用到上限，accept 返回 EMFILE，接受连接的线程退出。
  3. 与此同时，核心进程自己的网络重连、读本机文件也会失败。
  4. 之后内核里的 NFS 客户端一旦需要重连，就连不上，磁盘失效约一分钟。
  - 这是本机其他用户能做到的拒绝服务。
- 修复方向：
  - 暂时性错误时睡一小会儿，再继续循环。
  - 限制每台服务的并发连接数，内核客户端只用一两个连接，超出的直接关掉；空闲太久的连接也关掉。
  - 创建线程失败时接住异常，关掉这个连接。

## 疑点

### S1 写剪贴板时，系统如果查看"包"文件夹的内部，核心线程会卡住

- 位置：`src/lib/platform/OSXPasteboardFiles.mm:86`；等待逻辑在 `src/lib/platform/OSXRemoteFileSystem.cpp:447–500`
- 原文摘录：

  ```objc
      if (![pasteboard writeObjects:urls]) {
  ```

- 疑点：
  - `writeObjects:` 在核心线程上执行，期间系统会查看每个文件（交付文件第 4 节第 16 条）。按设计，顶层条目和清单目录用刚收到的清单回答，不用等对端。
  - 但如果系统还要看顶层文件夹的内部，就会出问题。例如名字以 `.app`、`.bundle`、`.rtfd` 结尾的文件夹，LaunchServices 可能要读 `Contents/Info.plist`，判断它是不是一个包。
  - 这时 LOOKUP 会进入 `list()`。第一次列这个目录要向对端要列表，列表请求得经事件队列发出，而核心线程正卡在 `writeObjects:` 里。
  - 于是 `list()` 等满 30 秒才回「稍后再试」，系统隔几秒重试。最长约两分半钟后回读写错误，期间键鼠一直不动。
  - 现有测试 `put_doesNotWaitForTheVolume` 只用了 `a.txt` 和一个普通文件夹，没覆盖包。系统到底会不会看包的内部，没有实测。
- 怎么验证：
  1. 在 Windows 上建一个 `X.app\Contents\Info.plist`，然后复制它。
  2. 在 Mac 端让对端卡住，再调用 put()。
  3. 看 put() 是否仍在 200 毫秒内返回，服务端有没有收到对 `X.app` 内部的 LOOKUP。

### S2 读访达写的文件引用时，要访问文件所在的卷，而这一步在核心线程上

- 位置：`src/lib/platform/OSXPasteboardFiles.mm:56`
- 原文摘录：

  ```objc
        NSURL *path = url.filePathURL;
  ```

- 疑点：
  - 访达写的是文件引用 URL（`file:///.file/id=…`）。`filePathURL` 要按卷号和文件号向文件系统求出路径。
  - 客户端拥有剪贴板时，每次光标离开 Mac 都会重新读一遍剪贴板。调用链是 `Client::leave` → `sendClipboard` → `Computer::getClipboard` → `getClipboardFiles`。
  - 假设用户复制的是 SMB 共享上的文件，之后共享没了响应，例如笔记本离开了办公室网络。这时解析路径可能一直卡到 SMB 超时，期间键鼠停住。
  - `filePathURL` 会不会真的这样阻塞，没有实测。
  - 同一批路径之后还会在 `FileTransfer::offerLocalFiles` 里逐个查大小和时间，那部分属于切片 01。

### S3 所有请求共用 48 个处理名额，写剪贴板引起的查看可能排在慢读请求后面

- 位置：`src/lib/platform/OSXNfsServer.cpp:503`；名额数在 `:50`
- 原文摘录：

  ```cpp
      m_changed.wait(lock, [this] { return m_stopping || m_runningCalls < kMaxRunningCalls; });
  ```

- 疑点：
  - 名额用满时，接收线程不再读这条连接。后来的请求要等某个读请求结束，最长约 30 秒。
  - 后来的请求里包括核心线程上 `writeObjects:` 引起的 GETATTR 和 LOOKUP，所以核心线程会一起卡住。
  - 要用满 48 个名额，得有好几份粘贴同时进行，而且对端都很慢，现实中不太可能。没有实测。

### S4 `writeObjects:` 失败时可能产生回声

- 位置：`src/lib/platform/OSXPasteboardFiles.mm:85–88`；回退路径在 `src/lib/platform/OSXComputer.mm:841–857`
- 原文摘录：

  ```objc
      [pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
  ```

- 疑点：
  - `prepareForNewContentsWithOptions:` 已经把剪贴板清空了。如果随后 `writeObjects:` 失败，put 返回 false，setClipboard 改走 `Clipboard::copy`。
  - 这条路不调用 `m_pasteboard.synchronize()`，`m_writtenAt` 也是 -1。
  - 旧剪贴板接口下一次 synchronize 时，如果把 NSPasteboard 那次清空报成"剪贴板变了"，Mac 就会向服务端报告"这边复制了新东西"，服务端随即清掉 Windows 剪贴板里的文件。这正是交付文件第 4 节第 17 条要防的情况。
  - 只在 `writeObjects:` 失败时才会发生。这种情况下旧接口报不报"变了"，没有实测。

### S5 退出后，剪贴板上仍是指向已删除挂载点的路径

- 位置：`src/lib/platform/OSXComputer.mm:870`（setFileTransfer 释放剪贴板文件对象的地方）
- 原文摘录：

  ```objc
    m_clipboardFiles.reset();
  ```

- 疑点：
  - Windows 端退出时会清掉剪贴板上自己放的虚拟文件（交付文件第 4 节第 25 条），Mac 端没有对应的处理。
  - Deskflow 退出后，用户在访达里按粘贴，会提示找不到原项目。
  - 如果上次的挂载没卸掉（例如进程崩溃或被强制结束），访达还会在已失效的挂载上卡几秒。
  - 要不要处理，取决于产品意图。

### S6 对端完全不发数据时，同一位置第二次读很可能直接报错，只容忍约 34 秒

- 位置：
  - `src/lib/platform/OSXRemoteFileSystem.cpp:786`：读请求遇到已失败的下载，直接回失败。
  - 同文件 541–546：pickStream 也会选中已失败、但还没停掉的下载。
  - 同文件 699–700：下载的失败会被记住 failureMs。
  - 同文件 270–287：touch 只在 3 秒内没有任何调用时，才开新的一次查看并停掉闲着的下载。
  - 默认参数：`OSXRemoteFileSystem.h:65`（waitMs = 30000）、`:75`（failureMs = 5000）；`FileTransfer.cpp:126`（replyTimeoutMs = 30000）。
- 原文摘录：

  ```cpp
          if (available == 0 && stream->failed) {
  ```

- 疑点：
  - 交付文件第 4 节第 13 条写的是"最多等 30 秒才回「稍后再试」，同一位置最多回 3 次，之后报读写错误"。对端完全不发数据时，按默认参数推算是另一回事：
    1. 读者的无活动超时（30 秒）和读请求的等待上限（30 秒）几乎同时到。读请求先回「稍后再试」，下载随后以超时失败。
    2. 这次失败被记住 5 秒。
    3. 系统约 4 秒后重试同一位置。这时失败的下载还在，pickStream 仍会选中它，于是第二次就回读写错误。
  - 结果是访达约 34 秒就报错，而不是按第 13 条字面推算的约两分半钟。
  - 例外：两次之间如果 3 秒以上没有任何调用碰过这份清单，重试时 touch() 会开新的一次查看、停掉这条下载，重试就会重新下载并再等 30 秒。所以实际结果取决于期间有没有别的调用，例如 actimeo=1 带来的 GETATTR，不确定。
  - 测试表里另有一句"对端停止应答或断开时读请求及时报错"，约 34 秒报错也可能正是想要的。需要定下来哪种是预期。
- 怎么验证：
  1. 在 OSXRemoteFileSystemTests 里让对端一直不发数据，记录第几次读、多少秒后回 Io。
  2. 再在每次重试之间插一次 GETATTR，对比结果。
