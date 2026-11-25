# LanDrop 局域网快传

LanDrop 是一个用来在局域网内直接互传文件的桌面工具：拖入文件或文件夹、填写对方 IP、点击发送即可。Windows 版本是原生 Win32 程序，运行时静态链接，不需要安装 VC++ 运行库或额外 DLL，适合直接拷贝到干净的 Windows 机器上双击运行。

## 功能一览

- 拖拽文件、文件夹或混合内容到应用内即可加入发送队列。
- 双方同时运行 TCP 服务，支持 A → B 和 B → A 双向传输，也支持一台电脑同时收、发。
- 自动发现同网段设备，同时保留手动输入 IP；可填写 `IP:端口`。
- 文件夹递归传输，保留相对目录层级和空目录。
- 大文件分块读写（256 KiB），64 位文件偏移，不把整文件读入内存。
- SHA-256 完整性校验：发送前计算摘要，接收端校验通过后才将临时文件原子重命名为最终文件。
- 断点续传：连接断开时自动重连并继续；退出后重选同一批文件也可以从已有临时数据继续。若最终文件已经完整存在且 SHA-256 一致，接收端直接跳过重传。
- 临时文件放在接收目录的 `.landrop-parts/` 中，失败/中断时保留，成功后自动清理。
- 发送端跳过符号链接和 Windows 目录重解析点（junction）；接收端拒绝目标路径上的符号链接/重解析点，防止写入被重定向到接收目录之外。
- 异步非阻塞 socket + 后台工作线程：准备、校验、传输和接收期间界面保持可响应。
- 传输列表显示方向、设备、当前项目、进度、速度、状态；可取消本机发起的任务。
- 接收目录和“允许接收”开关保存在用户配置目录。
- 友好的错误提示和启动诊断；Windows 界面提供内置日志查看，Linux 在底部状态行显示最近提示/错误。

## 系统兼容性

| 平台 | 说明 |
|------|------|
| Windows 7 / 8.1 / 10 / 11 | 面向 Win7 及以上 API 编写；原生 Win32 UI；32 位构建可运行于 32/64 位 Windows |
| Linux | 原生 X11 + Xft 界面；支持 X11 桌面、中文显示、XDND 拖放 |
| 网络 | IPv4 局域网；TCP 47653，UDP 47654（设备发现） |

当前版本已在 Windows 11（64 位系统）上完成编译、核心测试和双实例 GUI 实机传输验证。Windows 7/8.1 与 Linux 图形界面建议在对应目标机器上做一次最终现场验证；Windows 7 的导入 API 已按 Win7 避免使用 Win8/Win10 专属接口。

### “干净系统双击即用”的说明

Windows 发布文件为单个 `LanDrop.exe`（约 2 MB）：

- 静态链接 MinGW C++ 运行库，不依赖 `libstdc++-6.dll`、`libgcc_s_dw2-1.dll`、`libwinpthread-1.dll`、VC++ Redistributable。
- 仅依赖 Windows 自带的 `KERNEL32`、`USER32`、`GDI32`、`WS2_32`、`SHELL32`、`COMDLG32`、`COMCTL32` 等系统组件。
- 不需要管理员权限；采用 `asInvoker`。
- 未做代码签名，首次运行时 SmartScreen/杀毒软件可能提示，属正常现象。

Linux 版本是常规动态 ELF，依赖系统桌面的 X11、Xft、fontconfig（正常桌面环境自带）；见下方构建说明。

## 快速使用

1. 两台电脑连接到同一个局域网。
2. 分别在两台电脑上打开 LanDrop。
3. Windows 首次启动时若弹出防火墙提示，请允许“专用网络”访问；否则对端无法连接。
4. 接收端确认：
   - “允许其他设备发送文件”已勾选；
   - 接收目录正确（默认 Windows 为 `%USERPROFILE%\Downloads\LanDrop`，Linux 为 `~/Downloads/LanDrop` 或用户主目录下的 LanDrop）；
   - 顶部能看到本机 IPv4 和监听端口。
5. 发送端：
   - 把文件/文件夹拖到窗口任意位置，或点击“选择文件”“选择文件夹”；
   - 手动输入目标 IP，或从“发现的设备”下拉框选择；
   - 点击“发送”。
6. 传输完成后，文件位于接收端所选目录；嵌套文件夹结构会保留。

> 也可以把文件/文件夹拖动到 `LanDrop.exe` 图标上启动，程序会把它们预加载到发送列表。
>
> 端口被占用时，第二个实例会进入“仅发送”模式并显示明确提示；两个 LanDrop 实例不要同时承担同一台机器的接收服务。

## 构建

### Windows（MinGW-w64）

本项目已在以下环境验证：

- 工具链：`D:\software\mingw32\bin`
- 编译器：`i686-w64-mingw32-g++` 16.2.0（MinGW-builds，32 位）
- 产物：`build\LanDrop.exe`（Win32 GUI 子系统，静态运行库）

最简单的方式是在项目根目录运行：

```bat
build_windows.bat
```

脚本默认使用 `D:\software\mingw32\bin`，如果工具链在别处：

```bat
set MINGW_BIN=D:\your\mingw64\bin
build_windows.bat
```

也可以手动构建：

```bat
set "PATH=D:\software\mingw32\bin;%PATH%"
mingw32-make -f Makefile.win
```

PowerShell 中：

```powershell
$env:PATH = "D:\software\mingw32\bin;$env:PATH"
mingw32-make -f Makefile.win
```

产物与相关命令：

| 命令 | 产物/动作 |
|------|-----------|
| `mingw32-make -f Makefile.win` | `build\LanDrop.exe` |
| `mingw32-make -f Makefile.win test` | 构建并运行 `build\LanDropTests.exe` |
| `mingw32-make -f Makefile.win clean` | 删除 `build\` |
| `build_windows.bat test` | 简写测试入口 |

**64 位 Windows 构建：** 安装/切换一套 64 位 MinGW-w64，将它的 `bin` 放到 `PATH` 第一位，再执行同样的 `mingw32-make -f Makefile.win`。当前提供的 32 位 exe 本身可以在 64 位 Windows 的 WOW64 下运行，因此一个 32 位发布包即可覆盖 32/64 位 Windows。

可用如下命令确认产物没有第三方 DLL 依赖：

```bat
objdump -p build\LanDrop.exe | findstr "DLL Name"
```

### Linux（X11 / Xft）

依赖：

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential pkg-config libx11-dev libxft-dev libfontconfig1-dev

# Fedora
sudo dnf install gcc-c++ make pkg-config libX11-devel libXft-devel fontconfig-devel
```

需要 C++17 编译器（GCC 8+；GCC 8 的 `std::filesystem` 由 Makefile 自动追加 `-lstdc++fs`）。Ubuntu 18.04 默认 `g++` 为 7，需额外安装 `g++-8`（`build_linux.sh` 会自动选择）；更新的发行版通常自带 GCC 9+。构建与运行：

```bash
bash build_linux.sh
./build/landrop
```

`build_linux.sh` 会在 `g++` 版本过旧时自动尝试 `g++-8` ~ `g++-12`；也可以显式指定：

```bash
CXX=g++-12 make
```

测试：

```bash
bash build_linux.sh test
```

安装到 `/usr/local/bin`（可选）：

```bash
sudo make install
```

32 位 Linux 构建需要 multilib 工具链及 32 位 X11/Xft 开发库：

```bash
make ARCH_FLAGS=-m32
```

说明：

- Linux 图形界面使用原生 X11 + Xft，没有 GTK/Qt 运行时依赖。
- “选择文件/文件夹”按钮优先调用 `zenity`，其次 `kdialog`；两者都不存在时仍可使用拖拽发送，并可直接编辑接收目录后按 Enter。
- 中文界面需要系统安装中文字体（如 `fonts-noto-cjk`）；没有 CJK 字体时中文可能显示为方框。
- **WSL 默认没有 X11 显示，无法直接运行 GUI。** 请使用 WSLg、配置 X server 或真实 Linux 桌面。核心网络层可独立通过测试二进制验证。
- Linux 端已完成编译、核心测试及 ASan/UBSan、ThreadSanitizer 验证；图形交互建议在目标 Linux 桌面上做一次现场验收（本机开发环境只有 WSL 命令行）。

## 断点续传与完整性校验

### 接收流程

1. 接收端先创建父目录，再把数据写入 `.landrop-parts/<hash>.part`。
2. 同时维护 `.part.meta`，记录期望文件大小与 SHA-256。
3. 普通新文件边收边计算摘要；续传文件在写完后重新读取整个临时文件计算 SHA-256（避免信任上一次进程遗留的字节）。
4. 摘要一致后关闭并 `fsync` 临时文件，然后原子替换为最终文件；元数据随后删除。
5. 摘要不一致时删除损坏的临时文件和元数据，避免后续续传继续使用坏数据；断线、取消或协议错误则保留已经写入的部分数据，供下一次续传。

### 断线与自动重试

发送端在连接中断、超时、接收端忙碌或校验失败时自动重试，各类原因分别计数：

| 原因 | 最大重试次数 | 重试间隔 |
|------|--------------|----------|
| 网络中断/超时 | 3 | 2、4、5 秒 |
| 接收端忙碌（路径被其他任务占用） | 8 | 3、6、9…最多 15 秒 |
| 接收端校验失败 | 1 | 1 秒 |

网络读写空闲超时为 30 秒；接收端在校验大文件时会持续发送心跳，避免被误判为断线。接收端保留已写入的临时文件，重连后通过协议汇报续传偏移。任务失败或程序退出后，已写入数据仍留在 `.landrop-parts/`；再次选择同一文件发送即可继续。

**何时无法续传：**

- 源文件内容已改变，导致期望 SHA-256 变化；
- 接收目录改变；
- 临时目录被手动清理或损坏；
- 源文件/目标路径不再对应原来的文件。

出现这些情况时，旧临时数据会被判定为不匹配并重新从 0 开始传输，不会把新内容拼接到旧数据后面。

**清理方法：** 关闭 LanDrop 后删除接收目录下的 `.landrop-parts/` 即可清空所有未完成缓存。正常完成的传输不会留下缓存文件。

## 端口、防火墙与安全边界

- 固定端口：TCP `47653`（传输）、UDP `47654`（发现）；目标地址支持 `192.168.1.5` 或 `192.168.1.5:47653`。
- Windows 防火墙需放行专用网络；企业网络若启用 AP 隔离/客户端隔离，设备间可能无法直连。
- **没有加密、认证或密码/PIN。** 收到传输请求会自动接收（可在 UI 关闭接收）。该工具面向可信局域网，不建议在公共 Wi-Fi 或公网暴露端口。
- 接收端有路径穿越与符号链接逃逸防护；会拒绝目标路径上已存在的符号链接、Windows 重解析点、危险组件、重复路径、文件/目录冲突和超出接收根目录的路径。
- 同名最终文件会在新数据完整校验后被覆盖；若已有文件大小和 SHA-256 与待传文件一致，则直接跳过传输。没有“保留两者”的冲突对话框。
- 当前仅支持 IPv4 传输；目标可为主机名（由系统解析），但 UI 不做 IPv6。
- 默认最多并行处理 3 个入站连接；额外的短连接会排队。
- 单次清单最多 200,000 个条目；单条相对路径最大 4096 字节；清单路径总量上限为 16 MiB、1,000,000 个路径组件、单条路径深度 128。
- Windows 上最终目标路径限制为 248 个字符，续传缓存路径也会提前检查长度并给出明确错误。
- 不保留文件权限、ACL、扩展属性或原始时间戳；符号链接和 Windows 目录重解析点会被跳过并记录警告。

## 配置与日志

- Windows：`%APPDATA%\LanDrop\settings.ini`
- Linux：`$XDG_CONFIG_HOME/landrop/settings.ini` 或 `~/.config/landrop/settings.ini`

配置项：

```ini
receive_dir=C:/Users/name/Downloads/LanDrop
allow_incoming=1
```

Windows 界面提供“日志”按钮查看最近运行日志（可 Ctrl+C 复制）。Linux 版本将最近错误/提示显示在窗口底部状态行。

## 架构与工程结构

```
LanDrop/
├── Makefile                      # Linux
├── Makefile.win                  # Windows MinGW-w64
├── build_windows.bat
├── build_linux.sh
├── resources/                    # app.ico / app.manifest / app.rc
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── protocol.*            # 帧协议与消息编解码
│   │   ├── socket.*              # 非阻塞跨平台 socket
│   │   ├── scanner.*             # 递归扫描 + 预计算 SHA-256
│   │   ├── sender.*              # 发送状态机、自动续传重连
│   │   ├── receiver.*            # 监听、清单校验、接收/校验/原子落盘
│   │   ├── discovery.*           # UDP 局域网设备发现
│   │   ├── state.*               # 线程安全 UI 状态/日志/任务列表
│   │   ├── app_controller.*      # 生命周期与任务编排
│   │   ├── sha256.*              # 无依赖 SHA-256
│   │   └── settings.*
│   ├── platform/                 # Windows / POSIX 平台层
│   └── ui/
│       ├── ui_win.cpp            # 原生 Win32 界面
│       └── ui_x11.cpp            # Xlib + Xft 界面，含 XDND 拖放
├── tests/test_core.cpp           # 单元 + 端到端 + 断线续传 + 安全测试
├── tools/                        # 图标生成、Windows GUI 冒烟测试
├── docs/PROTOCOL.md              # 私有协议 v1 说明
└── README.md
```

设计要点：

- **UI 与网络完全解耦**：核心状态由互斥量保护，UI 以 5 Hz 拉取快照；网络/磁盘工作全部在后台线程。
- **非阻塞 socket**：所有连接使用非阻塞 socket + `select` 分段等待，并定期检查取消/退出标志，UI 不会因网络阻塞；大文件传输期间仍能操作界面和取消任务。
- **帧级边界校验**：固定魔数、版本、最大帧长、严格长度判断、索引校验、文件大小上限，防止协议解析越界。
- **分块哈希/落盘**：接收时按块写入，不在内存中保留整个文件。
- **原子提交**：先在同目录写临时文件，校验通过后再替换最终路径，避免半成品暴露。
- **平台原生**：Windows 使用 Win32 控件与 GDI 绘制，Linux 使用 Xlib/Xft；不引入 Qt/wxWidgets 等大型运行时。

协议细节见 [docs/PROTOCOL.md](docs/PROTOCOL.md)。

## 测试

核心测试不依赖 GUI，覆盖：

- SHA-256 标准向量与分段输入；
- 相对路径校验与路径穿越拒绝；
- 协议消息/64 位偏移往返；
- 递归文件夹、空目录、零字节文件、含空格文件名、Unicode 文件名和 12 MB 文件端到端传输；
- Unicode 大小写冲突的顶层名称去重；
- 已存在且 SHA-256 一致的最终文件跳过重传；
- 模拟接收端已有部分文件后的断点续传；
- 通过 TCP 截断代理验证“连接中断 → 自动重连 → 续传”真实路径；
- 接收端在数据流中报告磁盘错误时，发送端显示远端原因而不是笼统的断线；
- 设备发现启动、收到广播以及及时关闭；
- 双向发送与接收；
- 拒绝 `../` 路径穿越且确认接收根目录之外没有写入。

运行：

```bat
build_windows.bat test
```

```bash
bash build_linux.sh test
```

当前测试结果：Windows `140 checks, 0 failures`；Linux `139 checks, 0 failures`（差异为 Windows 专属的 UTF-8 错误文本测试）。Linux 下 ASan/UBSan 与 ThreadSanitizer 构建同样全部通过；在 WSL 中为规避 ASLR 与 sanitizer 的已知兼容问题，使用 `setarch $(uname -m) -R` 运行。

Windows GUI 双实例冒烟测试（需要可交互桌面，会启动两个实例并通过回环地址发送 24 MB 文件、校验 SHA-256、截图）：

```powershell
pwsh -File tools\ui_smoke_test.ps1
```

截图输出到 `build/ui-receiver.png` 与 `build/ui-sender.png`。

## 常见问题

**对方一直连接超时？**
检查双方是否同一网段、Windows 防火墙是否允许专用网络、企业 Wi-Fi 是否开启客户端隔离、端口 47653 是否被占用；可先在接收端本机用 127.0.0.1 自测。

**第二个实例提示“接收服务未启动”？**
端口被第一个实例占用，这是预期行为。第二个实例仍可发送；不要再让它承担接收。

**传输中断后文件不见了？**
最终文件只有在校验通过后才会出现，这是为了安全。中断数据在 `.landrop-parts/`，重新发送同一文件会自动续传；如果源文件已改变，会重新开始。

**磁盘空间/权限错误？**
接收端在写入前不做完整空间预留；磁盘满会保留未完成数据并报错，释放空间后重发同一文件可继续。接收目录必须可写。

**Linux 按钮没有反应/没有文件选择框？**
安装 `zenity` 或 `kdialog`，或直接把文件拖入窗口。接收目录字段可直接编辑后按 Enter。

**中文显示方框？**
安装中文字体（如 `fonts-noto-cjk`）后重新启动。

## 第三方组件与许可

代码自行实现 SHA-256、协议与 UI；不包含第三方库源码。Windows 使用系统 Win32/Common Controls，Linux 使用 X11/Xft/fontconfig。项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。
