# 审核切片 03：Windows 端（虚拟文件与剪贴板线程）

- 日期：2026-11-27
- 范围：提交 `80221f0f7..HEAD` 中 Windows 端的虚拟文件数据对象、远端文件来源、剪贴板线程、COM 安全设置，以及 `MSWindowsComputer`、`MSWindowsClipboard`、`deskflow-core.cpp` 的改动部分
- 方法：只读。没有编译、没有运行测试和程序。跨切片的代码只为判断行为而读，发现只报原因在本切片里的
- 依据分两类：代码能证明的，直接写；靠 Windows 平台行为（凭文档和经验，没有在本机验证）的前提，在各条里标「平台前提」

结论：高 1 条、中 2 条、低 5 条、疑点 5 条。最要紧的是 W-01（重新列出后，正在进行的拷贝可能把别的文件的内容写进去）和 W-02（事件线程看到写到一半的剪贴板，把本进程的写入当成用户复制）。

## 范围与读法

### 本切片的文件

| 文件 | 读法 | 说明 |
|---|---|---|
| `src/lib/platform/MSWindowsVirtualFileDataObject.h` | 完整读过（1-150） | 另对照了 `c62db8bc5..HEAD` 的改动 |
| `src/lib/platform/MSWindowsVirtualFileDataObject.cpp` | 完整读过（1-720） | 同上 |
| `src/lib/platform/MSWindowsRemoteFileSource.h` | 完整读过（1-80） | |
| `src/lib/platform/MSWindowsRemoteFileSource.cpp` | 完整读过（1-284） | |
| `src/lib/platform/MSWindowsClipboardFiles.h` | 完整读过（1-109） | |
| `src/lib/platform/MSWindowsClipboardFiles.cpp` | 完整读过（1-312） | |
| `src/lib/platform/MSWindowsComSecurity.h` | 完整读过（1-23） | |
| `src/lib/platform/MSWindowsComSecurity.cpp` | 完整读过（1-77） | |
| `src/lib/platform/MSWindowsComputer.h` | 略读：改动部分（`git diff 80221f0f7`）加 1-60、295-347 | 其余是与剪贴板无关的声明 |
| `src/lib/platform/MSWindowsComputer.cpp` | 略读：改动部分加 1-460、780-1000、1360-1510 | 读了构造与析构、剪贴板读写、`checkClipboards`、窗口与 `WM_CLIPBOARDUPDATE`、`onClipboardChange`。461-779、1001-1359、1511-1769 是按键、鼠标、屏保、热键，与本切片无关，未读 |
| `src/lib/platform/MSWindowsClipboard.h` | 完整读过（1-108） | |
| `src/lib/platform/MSWindowsClipboard.cpp` | 完整读过（1-226） | |
| `src/apps/deskflow-core/deskflow-core.cpp` | 完整读过（1-158） | |
| `src/lib/platform/MSWindowsClipboardAnyTextConverter.h/.cpp`、`MSWindowsClipboardUTF16Converter.h/.cpp`、`MSWindowsClipboardHTMLConverter.h/.cpp`、`MSWindowsClipboardBitmapConverter.cpp`、`MSWindowsClipboardFacade.h/.cpp`、`IMSWindowsClipboardFacade.h` | 完整读过 | 剪贴板线程用到的转换器 |
| `src/lib/platform/MSWindowsClipboardBitmapConverter.h` | 未读 | 只有声明，`.cpp` 已读 |
| `src/lib/platform/CMakeLists.txt` | 略读：改动部分 | |

### 为判断行为追过去的依赖（不在本切片报告范围）

| 文件 | 读法 |
|---|---|
| `src/lib/deskflow/FileTransfer.h` | 完整读过 |
| `src/lib/deskflow/FileTransfer.cpp` | 1-600 完整；另读 `startRequest`、`finishRequest`、`channelClosed`、`acceptRemoteFiles` 和 741 行的列目录选项 |
| `src/lib/deskflow/Computer.cpp` | 完整读过；`Computer.h`、`IPlatformComputer.h` 读改动部分 |
| `src/lib/deskflow/Clipboard.cpp`、`IClipboard.cpp` 的 `copy`（121-152） | 读过 |
| `src/lib/server/Server.cpp` | 440-520、1165-1235、1440-1495 |
| `src/lib/server/PrimaryClient.cpp`、`ClientProxy1_0.cpp`（241-258）、`ClientProxy1_6.cpp`（55-80） | 剪贴板相关方法 |
| `src/lib/client/Client.cpp` | 剪贴板相关行（244-263） |
| `src/lib/platform/OSXComputer.mm` | 841-895 |
| `src/lib/deskflow/ServerApp.cpp`、`App.cpp` | 生命周期相关段落 |
| `src/lib/base/Log.h`（165-220）、`Log.cpp` 的 `print` | 读过 |
| `src/unittests/platform/MSWindowsVirtualFileTests.cpp` | 1-300、380-557 完整；300-380 只看了断言 |
| `src/unittests/platform/MSWindowsClipboardFilesTests.cpp` | 完整读过 |
| `docs/plans/交付-剪贴板文件复制.md` 第 4-9 节、`docs/dev/protocol_reference.md` 286-331 行 | 读过 |

### 核对过、没有发现问题的点

- 引用计数：数据对象、流、自由线程封送器的聚合（内部 `IUnknown` 转发 `IID_IMarshal`、析构时释放）、`putFiles` 失败路径的 `Release`、`m_current` 的持有和释放都对。
- `OleSetClipboard` 只在已 `OleInitialize` 的剪贴板线程上调用；退出时用 `OleIsCurrentClipboard` 判断后 `OleSetClipboard(nullptr)`；全程没有 `OleFlushClipboard`。
- 三条写入路径（`putFiles`、`putFormats`、`empty`）写完的剪贴板都带归属格式。
- 向对端请求一律用原始远端名字（`remotePath` 由 `child.name` 拼成，`open` 用 `m_remote` 里存的路径）；改名只影响描述符里的名字。
- 相对路径按 UTF-16 码元数与 259 比较，超长的目录连同下面的内容一起跳过；描述符数组按目录在前、内容在后的顺序给出。
- `CoInitializeSecurity` 在 `QApplication` 之前调用，主线程先进单线程套间，与 Qt 随后的 `OleInitialize` 相容；安全描述符的属主、属组、DACL（IU/SY/BA，`0x3` 即本机调用和执行）与中等完整性标签格式正确，转换成绝对格式后的缓冲区在进程生命期内有效；出站调用用的身份级别与不调用时的默认值相同。
- 用 MSVC 默认的 `/W3` 加告警即报错核对了新增代码：没有发现 `size_t` 收窄、有符号与无符号比较、`min`/`max` 宏冲突、不完整类型的 `unique_ptr` 析构这类 GCC 语法检查发现不了的问题。

## 发现

### W-01 再次列出后，正在进行的拷贝按新列表取内容，可能把别的文件的内容写进去

- 级别：高
- 位置：`src/lib/platform/MSWindowsVirtualFileDataObject.cpp:413`、`432`、`500-510`、`696`、`717`；`src/lib/platform/MSWindowsRemoteFileSource.cpp:223-226`、`231-259`
- 原文摘录：

413 行：

```cpp
    const bool current = !fresh || m_inOperation || std::chrono::steady_clock::now() - m_listedAt < kListingLifetime;
```

432 行：

```cpp
  m_entries.clear();
```

500-503 行：

```cpp
  // the index is one of the descriptors handed out last
  if (!ensureListed(false)) {
    return E_FAIL;
  }
```

510 行：

```cpp
  const MSWindowsVirtualFileEntry &entry = m_entries[static_cast<size_t>(index)];
```

696 行（`StartOperation`）：

```cpp
  m_inOperation = true;
```

717 行（`EndOperation`）：

```cpp
  m_inOperation = false;
```

`MSWindowsRemoteFileSource.cpp` 223-226 行：

```cpp
    if (entry.isMissing) {
      LOG_INFO("a copied file is no longer there: %s", entry.name.c_str());
      continue;
    }
```

- 问题：`GetData(FileContents, lindex)` 的序号总是按最近一次列出的 `m_entries` 解析。资源管理器手里拿的是它自己那次取到的描述符数组。它拷贝期间只要另有一次描述符请求触发重新列出，`m_entries` 就被整体换掉，它后面请求的第 k 个文件就是新列表的第 k 项。`sourceId` 只保护已经创建出来的流，保护不了还没请求的文件。防止重新列出的条件 `m_inOperation` 是布尔量而不是计数：两次重叠的异步拷贝里，先结束的那次 `EndOperation` 就把它清掉，另一次还在拷。设计意图写在交付文件第 4 节第 26 条（「同一次拷贝进行中不重新列」）和测试注释里（`MSWindowsVirtualFileTests.cpp:251`，"the descriptors handed out must stay what they are"）。
  序号会不会变，由本切片的代码决定：顶层条目已不在时整项跳过（223-226 行），它之后的序号全部前移；广度优先列表把各目录的子项依次接在后面（231-259 行），任何一个先列的目录里多一项或少一项，排在它后面的所有目录的条目都跟着移位。
  另一个后果：重新列出失败（对端一时不答、超时）时 `m_listFailed` 置真，正在进行的拷贝之后每个文件的 `GetData(FileContents)` 都返回 `E_FAIL`：426-430 行的失败路径不清 `m_entries`，但 `ensureListed(false)` 返回 `!m_listFailed`。
- 触发条件：疑点 Q-01（剪贴板粘贴是否调用 `StartOperation`）不必先弄清，两种情况都有触发序列：
  - A（资源管理器不调 `StartOperation`，标志从不为真）：
    1. Mac 上复制两个文件夹 X、Y，在 Windows 的文件夹 A 粘贴，拷贝持续 3 秒以上；
    2. 拷贝进行中，用户在 Mac 上往 X 里新建或删除一个文件（或删掉被复制的某个顶层条目）；
    3. 在文件夹 B 再粘贴一次，或任何程序此时读 `FileGroupDescriptorW`，距上次列出已超过 3 秒，于是重新列出，Y 下所有条目的序号移位；
    4. A 的拷贝请求 Y 里下一个文件时拿到的是相邻的另一个文件，A 里这个文件名下写入了别的文件的内容，大小也不同，不报错。
  - B（资源管理器调 `StartOperation`）：A 粘贴（`StartOperation`）→ B 粘贴（列表沿用，`StartOperation`）→ A 结束（`EndOperation` 置假，B 仍在拷）→ Mac 上目录有变化 → 第三次描述符请求 → 重新列出 → B 后面的文件取错。
- 修复方向：已经交给某次粘贴的列表，在仍可能被使用时不要替换。`StartOperation`/`EndOperation` 改成计数；每次 `GetData(FileContents)` 和每个还活着的流都占住当前列表（例如记下最近一次取内容的时间、统计存活的流），没有拷贝在用时才重新列出；重新列出失败时不影响旧列表上的取内容。补一个用例：取描述符 → 取前几个文件 → 源变化后再取一次描述符 → 按旧序号继续取，校验内容。

### W-02 事件线程可能看到剪贴板线程写到一半的剪贴板，把本进程的写入当成用户复制

- 级别：中
- 位置：`src/lib/platform/MSWindowsComputer.cpp:962-969`、`1374-1379`、`374`、`325-329`；`src/lib/platform/MSWindowsClipboardFiles.cpp:98-101`、`241`、`267-276`；`src/lib/platform/MSWindowsClipboard.cpp:58`、`76`
- 原文摘录：

`MSWindowsComputer.cpp` 1374-1379 行：

```cpp
  if (!MSWindowsClipboard::isOwnedByDeskflow()) {
    if (m_ownClipboard) {
      LOG_DEBUG("clipboard changed: lost ownership");
      m_ownClipboard = false;
      sendClipboardEvent(EventTypes::ClipboardGrabbed, kClipboardClipboard);
      sendClipboardEvent(EventTypes::ClipboardGrabbed, kClipboardSelection);
```

`MSWindowsComputer.cpp` 325-329 行：

```cpp
  if (m_clipboardFiles != nullptr) {
    // one thread writes every clipboard, in order, files of other computers included
    m_clipboardFiles->set(src);
    return true;
  }
```

`MSWindowsClipboard.cpp` 58 行：

```cpp
  if (!EmptyClipboard()) {
```

`MSWindowsClipboard.cpp` 76 行：

```cpp
  if (nullptr == SetClipboardData(getOwnershipFormat(), data)) {
```

- 问题：代码能证明的部分：改动前，写剪贴板和判断归属都在事件线程上，前后串行；现在写入全在剪贴板线程上做（325-329 行），归属判断仍在事件线程上做（1374 行、374 行），两者之间没有任何同步。每次写入先 `EmptyClipboard`、后放归属格式（58、76 行；`OleSetClipboard` 路径在两者之间还要枚举全部格式）。写入总是成串的：对端每复制一次，两个剪贴板编号各发一次抢占（`OSXComputer.mm:891-892`），服务端对主屏各调一次 `grabClipboard`（`Server.cpp:1213`），这边就连着两个 `empty()`；数据到了又连着两次 `set()`（去重通常不生效，见 W-04）。这条路径对所有剪贴板写入都生效，纯文字、文件共享关闭时也一样，因为 `m_clipboardFiles` 总是存在。
  平台前提：`IsClipboardFormatAvailable` 不打开剪贴板，反映的是此刻的格式表；`EmptyClipboard` 一调用格式表就空了、序号就变了；`WM_CLIPBOARDUPDATE` 在写入方关闭剪贴板时发给监听者。据此，事件线程在后一次写入的清空与放归属格式之间处理前一次写入的通知（962-969 行），或在离开本机时调 `checkClipboards`（374 行），就会判定「失去归属」，向服务端报告用户复制了东西。
- 触发条件：
  1. Windows 剪贴板上是 Deskflow 写的内容（`m_ownClipboard` 为真），例如上一次从 Mac 收到的剪贴板；
  2. 用户在 Mac 上复制，Mac 发两次抢占，剪贴板线程连着执行两个 `empty()`；
  3. 第一个 `empty()` 的 `WM_CLIPBOARDUPDATE` 在第二个 `EmptyClipboard` 之后、`SetClipboardData(归属格式)` 之前被事件线程处理，`isOwnedByDeskflow()` 为假，发出 `ClipboardGrabbed`；
  4. 服务端总是允许主屏抢占（`Server.cpp:1183-1186`），把主屏记为拥有者，并让 Mac 抢占；Mac 的 `Client::grabClipboard` 把 `m_ownClipboard` 置假（`Client.cpp:263`），离开 Mac 时不再发送剪贴板（`Client.cpp:244`）；用户回到 Mac 时，服务端把空的主屏剪贴板发给 Mac（`ClientProxy1_6.cpp:58-79`），Mac 的剪贴板被清空。
  结果：用户在 Mac 上复制的内容在两台电脑上都没了。这是竞态，窗口是微秒级，但每次对端复制都会出现成串写入，长期使用中会碰到。
- 修复方向：归属判断不能落在写入中途。例如剪贴板线程维护「写入中」标志或已投递、已完成的计数，事件线程在有写入未完成时不下结论，等这批写入完成后再判断一次（剪贴板线程写完一批后给计算机窗口发一条消息即可）；或在判断时看 `GetOpenClipboardWindow()`、`GetClipboardOwner()` 是否属于剪贴板线程。`checkClipboards` 同样处理。

### W-03 写入失败也被当成写完：失败的内容不再重写，退出时不再把文件撤下

- 级别：中
- 位置：`src/lib/platform/MSWindowsClipboardFiles.cpp:256-265`、`267-276`、`241-247`、`111-113`、`157-176`、`185-187`；`src/lib/platform/MSWindowsClipboard.cpp:126-137`
- 原文摘录：

`MSWindowsClipboardFiles.cpp` 256-265 行：

```cpp
void MSWindowsClipboardFiles::putFormats(const IClipboard &clipboard)
{
  MSWindowsClipboard target(m_window);
  if (!Clipboard::copy(&target, &clipboard)) {
    LOG_WARN("failed to set the clipboard");
  }
  // emptying the clipboard dropped what was there before
  forgetCurrent();
  m_sequenceAfterWrite = GetClipboardSequenceNumber();
}
```

`MSWindowsClipboardFiles.cpp` 111-113 行：

```cpp
  if (data == m_lastData && written != 0 && written == GetClipboardSequenceNumber()) {
    return;
  }
```

`MSWindowsClipboard.cpp` 126-137 行：

```cpp
  for (int i = 0; i < kMaxRetries; ++i) {
    if (OpenClipboard(m_window)) {
      std::scoped_lock lock{m_mutex};
      m_time = time;
      return true;
    }

    if (i < kMaxRetries - 1) {
      LOG_DEBUG("failed to open clipboard (attempt %d/%d, error=%d), retrying", i + 1, kMaxRetries, GetLastError());
      Sleep(kRetryDelayMs);
    }
  }
```

`MSWindowsClipboardFiles.cpp` 185-187 行：

```cpp
  if (m_current != nullptr && OleIsCurrentClipboard(m_current) == S_OK) {
    OleSetClipboard(nullptr);
  }
```

- 问题：代码能证明的部分：`MSWindowsClipboard::open` 五次 `OpenClipboard` 都失败时（别的进程开着剪贴板超过约 20 毫秒），`Clipboard::copy` 返回假，剪贴板一点没变；`putFormats` 却照样 `forgetCurrent()`、记下当前序号，`empty()`（267-276 行）同样。`putFiles` 在 `OleSetClipboard` 失败时也退到 `putFormats`（241-247 行）。后果：
  - (a) 仍在剪贴板上的数据对象不再被记住，`m_current` 为空，退出时 185 行的条件不成立，文件留在剪贴板上进入 `OleUninitialize`，这是第 4 节第 25 条要避免的（OLE 此时怎么处理见疑点 Q-05）。退出时的 `OleSetClipboard(nullptr)` 也不看返回值，此刻剪贴板被占用同样会把对象留下。
  - (b) 失败那次的内容不会再被写上：它之后才到的同一份 `set()` 被 111-113 行的去重跳过（`written` 等于当前序号）；在它之前已经排进队列的同一份任务紧接着执行，两次之间不处理消息（157-164 行同一批任务连着执行；下一批到来时 `m_wake` 是序号 0 的对象，按 `MsgWaitForMultipleObjectsEx` 的文档先被满足，171 行的条件不成立，不会走到 173 行的 `PeekMessage`），占着剪贴板的一方若在等本线程，就同样失败。
  平台前提（未在本机验证）：别的程序读本进程经 OLE 延迟提供的格式（文字、HTML、归属标记）时，系统给剪贴板线程上的 OLE 窗口发 `WM_RENDERFORMAT`，对方开着剪贴板等它处理；`Sleep` 和 `OleSetClipboard` 都不处理发来的消息。据此，这种「打不开」恰好在剪贴板工具读上一份 OLE 内容的时候出现，而本线程不处理消息，对方就一直等到本线程放弃。
- 触发条件：
  1. 开着 Windows 剪贴板历史（Win+V），或任何在剪贴板变化时读取文字的工具；
  2. Mac 上在访达里复制文件（会带文件名的文字），回到 Windows；服务端给两个编号连着 `set()`，第二次不被去重（W-04）；
  3. 第一次 `OleSetClipboard` 完成，剪贴板工具打开剪贴板取 `CF_UNICODETEXT`，等剪贴板线程渲染；
  4. 剪贴板线程正做第二次写入：`OleSetClipboard` 打不开剪贴板而失败 → `putFormats` 重试 5 次失败 → `forgetCurrent()`。第一份数据对象仍在剪贴板上但不再被记住，退出 Deskflow 时文件不会被撤下。
  若失败的那次带的是新内容（下一份剪贴板恰在剪贴板工具读上一份时到达），这份新内容就不会出现在 Windows 剪贴板上，粘贴出来的仍是上一份。
- 修复方向：只有真的清空或写入成功后才 `forgetCurrent()` 和记录序号；失败时清掉 `m_lastData`，让下一次 `set()` 重写，或在剪贴板线程里稍后重试；等剪贴板时用 `MsgWaitForMultipleObjects` 处理发来的消息而不是 `Sleep`；检查退出时 `OleSetClipboard(nullptr)` 的结果，失败就重试。

### W-04 两个剪贴板编号的去重几乎不起作用，每份带文件的剪贴板都写两次

- 级别：低
- 位置：`src/lib/platform/MSWindowsClipboardFiles.cpp:109-115`
- 原文摘录：

```cpp
  // the same clipboard arrives once for each clipboard ID, write it only once unless it was replaced since
  const DWORD written = m_sequenceAfterWrite;
  if (data == m_lastData && written != 0 && written == GetClipboardSequenceNumber()) {
    return;
  }
  m_lastData = std::move(data);
  m_sequenceAfterWrite = 0;
```

- 问题：`m_sequenceAfterWrite` 在投递时清零，要等剪贴板线程做完这次写入才填上。两个编号的数据连着到达（`Server.cpp:1486` 每个编号一次，两条消息只隔微秒），而 `OleSetClipboard` 要建窗口、封送数据对象，慢得多，第二次 `set()` 看到的 `written` 几乎总是 0，于是再投一次。测试 `clipboard_sameClipboardTwice_putsFilesOnce` 在两次 `set()` 之间调了 `waitUntilDone`，掩盖了这一点。
- 触发条件：Mac 复制文件后回到 Windows，两个编号的剪贴板背靠背到达 → 两次 `OleSetClipboard`、两个数据对象、两次剪贴板变化通知（剪贴板历史里多一条），并为 W-02、W-03 提供了时间上的重叠。
- 修复方向：按已投递的内容去重：上一个已投递的任务内容相同、之后剪贴板没被别人改过，就跳过；不要依赖写入完成后才有的序号。

### W-05 剪贴板线程被粘贴调用占住时，写入任务和别的程序的粘贴都要等

- 级别：低
- 位置：`src/lib/platform/MSWindowsClipboardFiles.cpp:170-176`；`src/lib/platform/MSWindowsVirtualFileDataObject.cpp:65-67`、`131`
- 原文摘录：

`MSWindowsClipboardFiles.cpp` 170-176 行：

```cpp
    const DWORD woken = MsgWaitForMultipleObjectsEx(1, &m_wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (woken == WAIT_OBJECT_0 + 1) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
```

`MSWindowsVirtualFileDataObject.cpp` 65-67 行：

```cpp
The stream aggregates the free threaded marshaler so that an application
copying on a background thread reads without going through the message loop
of the thread that owns the clipboard.
```

`MSWindowsVirtualFileDataObject.cpp` 131 行：

```cpp
    while (total < size && m_position < m_entry.size) {
```

- 问题：交付文件第 4 节第 23 条写明，资源管理器读虚拟文件的 COM 调用进到放数据的那条线程；流上的注释说可以绕开消息循环，只对本进程内的调用成立。代码能证明的部分：`run()` 只有在 `PeekMessage` 取空队列后才回去拿任务（173 行的循环）；`Read` 要凑满调用方要的字节才返回（131 行）；`list()` 按目录逐个往返。这期间：
  - (a) 新的写入任务只能排队；两个粘贴同时进行时总有一个调用在排队，任务可能要等到拷贝结束才执行；
  - (b) 用户在这期间在 Windows 上复制了东西，排队的旧任务随后执行，把用户刚复制的内容覆盖掉，并打上 Deskflow 的标记；
  - (c) 平台前提：别的程序粘贴这份剪贴板里的文字时，`WM_RENDERFORMAT` 发到这条线程，要等当前 `Read` 返回，对端卡住时最长 30 秒（应答超时，`FileTransfer.cpp:126`）。
- 触发条件：
  - (a) 在两个文件夹里同时粘贴大文件，期间在 Mac 上复制一段文字再回到 Windows 按粘贴：得到的仍是旧内容，直到两个拷贝都结束。
  - (b) Mac 弹出隐私授权框，读文件的工作线程卡住（交付文件第 7 节），Windows 上资源管理器的 `Read` 在等；期间 Mac 上复制过东西（两个 `empty()` 排队），用户回到 Windows 复制一段文字；`Read` 返回后排队的 `empty()` 执行，用户复制的文字被清掉。
  - (c) 同 (b) 的卡住状态下，在记事本里粘贴，记事本无响应，直到这次 `Read` 返回。
- 修复方向：每分发一条消息就检查一次任务队列；考虑让阻塞的读不占用这条套间线程（例如流在多线程套间里提供，或在套间里只做不阻塞的部分），至少让 `WM_RENDERFORMAT` 和写入任务不被读文件挡住。

### W-06 条目数到上限后，仍把排队的目录逐个向对端列一遍

- 级别：低
- 位置：`src/lib/platform/MSWindowsRemoteFileSource.cpp:204-207`、`231-237`
- 原文摘录：

204-207 行：

```cpp
    if (entries.size() >= m_limits.maxEntries) {
      ++leftOut;
      return;
    }
```

231-237 行：

```cpp
  while (!directories.empty()) {
    const Directory directory = std::move(directories.front());
    directories.pop_front();

    std::vector<ClipboardFileEntry> children;
    const FileTransferStatus listed =
        m_files->listDirectory(m_list.offerId, directory.item, directory.remotePath, children);
```

- 问题：到上限后新条目不再加入，但已经排进队列的目录照样逐个列出，子项全部丢弃。队列里最多可有 65536 个目录，每个一次网络往返，这期间剪贴板线程和资源管理器都在等，换来的是被截断的粘贴。
- 触发条件：复制一棵超过 65536 项、目录很多的树（例如带 `node_modules` 的项目），在 Windows 上粘贴：上限在前几层就达到，之后仍有成千上万次 `listDirectory`；局域网每次几毫秒，合计以分钟计，期间资源管理器一直停在准备阶段。
- 修复方向：达到上限后清空队列、停止列目录，日志里写明被跳过的数量。

### W-07 每列一次就给 `m_remote` 追加整棵树，数据对象留在剪贴板上期间只增不减

- 级别：低
- 位置：`src/lib/platform/MSWindowsRemoteFileSource.cpp:162-167`、`213`
- 原文摘录：

```cpp
size_t MSWindowsRemoteFileSource::remember(uint32_t item, const std::string &relativePath)
{
  std::scoped_lock lock{m_mutex};
  m_remote.push_back({item, relativePath});
  return m_remote.size() - 1;
}
```

- 问题：每次 `list()` 都给全部条目新分配编号并保存远端路径，旧的从不释放，直到数据对象和所有流都被释放。
- 触发条件：剪贴板上放着一棵 6 万项的树，用户反复粘贴，或某个程序隔几秒读一次 `FileGroupDescriptorW`（每次间隔超过 3 秒就重新列出）：每次多出几 MB（每项一个 `std::string` 路径），同时每次都是一次完整的远端列目录。
- 修复方向：同一远端路径复用已有编号（以条目序号加路径为键的映射）。

### W-08 保留设备名不全

- 级别：低
- 位置：`src/lib/platform/MSWindowsRemoteFileSource.cpp:60`、`67-68`
- 原文摘录：

60 行：

```cpp
  const std::wstring base = folded(name.substr(0, name.find(L'.')));
```

67-68 行：

```cpp
  if (base.size() == 4 && (base.compare(0, 3, L"com") == 0 || base.compare(0, 3, L"lpt") == 0) && base[3] >= L'1' &&
      base[3] <= L'9') {
```

- 问题：代码能证明的部分：编号位只接受 ASCII 的 1 到 9；基名在第一个点之前截取，没有去掉点前的空格；测试 `windowsName_changesWhatWindowsDoesNotAllow` 断言 `COM0` 不改。
  平台前提（凭微软的文件命名文档）：上标数字 `¹ ² ³` 也算 COM、LPT 的编号，`COM¹`、`LPT²` 是保留名；保留名单还列了 `COM0`、`LPT0`；解析设备名时点前的空格被忽略，`CON .txt` 等同于 `CON`（至少到 Windows 10）。
- 触发条件：Mac 上复制名为 `COM¹.txt`、`LPT².log` 或 `NUL .txt` 的文件，在 Windows 上粘贴：资源管理器创建这个文件时打开的是设备，设备不存在就报错，存在就写到设备上，这个文件粘不出来。
- 修复方向：基名去掉尾部空格后再比较；编号位接受 `'0'`-`'9'` 和 U+00B9、U+00B2、U+00B3。

## 疑点

### Q-01 剪贴板粘贴是否走 `IDataObjectAsyncCapability`

- 位置：`src/lib/platform/MSWindowsVirtualFileDataObject.cpp:693`
- 原文：`IFACEMETHODIMP MSWindowsVirtualFileDataObject::StartOperation(IBindCtx *)`
- 说明：CI 上的两个 shell 测试（`shellCopiesVirtualFiles`、`shell_pastesTheFilesOfTheOtherComputer`）是直接拿原始数据对象调文件夹的 `DragEnter`/`Drop`，没有经过 `OleGetClipboard`。真实粘贴时资源管理器拿到的是 OLE 的剪贴板包装对象，它是否把 `IDataObjectAsyncCapability` 转给本对象、资源管理器是否调用 `StartOperation`，没有证据。W-01 两种情况都成立，这一点只影响触发的难易。验证办法：在 Windows 上用资源管理器粘贴，看日志里有没有 `paste of virtual files finished`（`EndOperation` 的 `LOG_DEBUG`）。

### Q-02 停止或重置时 `join` 可能等满 30 秒

- 位置：`src/lib/platform/MSWindowsClipboardFiles.cpp:65`
- 原文：`m_thread.join();`
- 说明：`~MSWindowsClipboardFiles` 在事件线程上 `join` 剪贴板线程（`Computer.cpp:99` → `MSWindowsComputer.cpp:355` → 本行）。连接关闭会唤醒在途请求（`FileTransfer::channelClosed`）。但若剪贴板线程在连接关闭之后、退出之前又处理了一个新的读调用，`openFile` 把 `startRequest` 投进事件队列，事件线程正在 `join`，不会处理，读方要等应答超时 30 秒（`FileTransfer.cpp:126`、`249`），停止或 `resetServer` 随之推迟。需要资源管理器恰在此时发新调用，未证实。

### Q-03 W-02 误报之后，事件线程可能与剪贴板线程互等

- 位置：`src/lib/platform/MSWindowsComputer.cpp:447`
- 原文：`Clipboard::copy(dst, &src);`
- 说明：W-02 的误报若发生在 Mac 为活动屏时，服务端立刻读主屏剪贴板（`Server.cpp:1217-1219`）。事件线程对本对象延迟提供的文字调用 `GetClipboardData`，平台前提是系统给剪贴板线程发 `WM_RENDERFORMAT` 并等待。若剪贴板线程此刻正阻塞在等事件线程送数据的 `Read` 里，两边互等，直到 30 秒应答超时，期间键鼠停顿。需要几件事同时发生，未证实。

### Q-04 以 SYSTEM 运行时，交互用户可让核心进程无限占内存

- 位置：`src/lib/platform/MSWindowsVirtualFileDataObject.cpp:630`
- 原文：`addGlobalFormat(format->cfFormat, data, size);`
- 说明：`CoInitializeSecurity` 之后，任何交互用户（中等完整性以上）都能调用本进程暴露的 COM 对象。数据对象的 `SetData` 接受任意格式、任意大小的 `HGLOBAL`，一直存在 `m_globalFormats` 里（611-637 行）。同一会话的用户进程可借此让以 SYSTEM 运行的核心进程不断占用内存。只能拒绝服务、不越权，影响限于本人，记为疑点。

### Q-05 数据对象还在剪贴板上时 `OleUninitialize` 会怎样

- 位置：`src/lib/platform/MSWindowsClipboardFiles.cpp:193`
- 原文：`OleUninitialize();`
- 说明：W-03 的后果是剪贴板线程带着仍在剪贴板上的数据对象调用 `OleUninitialize`。OLE 销毁自己的剪贴板窗口时，是否经 `WM_RENDERALLFORMATS` 把所有延迟格式渲染一遍（这会调 `GetData(FileGroupDescriptorW)`，列出整棵树；此时事件循环已停，每个请求要等 30 秒超时），还是留下失效的格式，没有查到权威说明，要在 Windows 上实测。
