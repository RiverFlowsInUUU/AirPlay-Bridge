# 开发笔记

给要改这个项目的人看的。使用说明在 [README.md](README.md)。

这里记的是开发过程中真踩过的坑 —— 每条都是花时间换来的，
写下来免得下次重蹈覆辙。

这一节是给**下一个动这个仓库的人（或 AI）**看的。以下每一条都是实际踩过的坑，
不是理论提醒。按顺序看完能省掉几个小时。

### 1. 改了延迟值，就要同步改测试

默认延迟在 `patches/upstream-airplay2-sender-cpp.patch` 里从上游的 **1.5s（66150 帧）**
调成了 **150ms（6615 帧）**。这个值会同时影响三处 RTP 握手参数：

| 参数 | 上游原值 | 本项目的值（150ms 时） |
|---|---|---|
| RTP 时间戳基准 / `rtptime` | 66150 | 6615 |
| `latencyMax`（允许接收端缓冲上限） | 88200 | `latency_ × 2` = 13230 |
| `latencyMin`（下限） | 11025 | `latency_ ÷ 3` = 2205 |

上游测试 `test/core_tests.cpp` 里这些值是**写死的**。我们已经在 patch 里把它们
改成跟随 `sender->latencyMs()` 动态计算，所以**你再调延迟不用改测试**。
但如果有人把这套动态计算改回去，`ctest` 会立刻报 5 项失败（`226/231`），
症状是 `rtptime` / `latency window` 相关的断言全红。

**改延迟的正确姿势**：只动 `raop_sender.h` 里的 `latency_` 默认值，
其余（RTP 基准、latency 窗口、测试期望）都是派生出来的。

### 2. 别让 `core.autocrlf` 把补丁改坏

`patches/*.patch` 是 unified diff，上下文行必须跟上游源文件**逐字节**吻合。
Windows 上 `core.autocrlf=true`（默认）会在 checkout 时把 patch 转成 CRLF →
`git apply` 直接失败（`patch does not apply`）→ **任何人都编译不了**。

仓库根目录的 `.gitattributes` 已经声明 `*.patch -text` 锁死行尾。
**不要删这个文件**，也不要往 patch 里混入 CRLF。

判断是否中招：

```bash
file patches/upstream-airplay2-sender-cpp.patch
# 正常: ASCII text / UTF-8 text
# 中招: ... with CRLF line terminators
```

### 3. Mbed TLS 为什么是 3.6.0

版本由**上游写死**（`FetchContent` 里 pin 了一个 commit，注释标明 v3.6.0），
不是我们选的。实测 3.6.2 **也能编译并通过全部测试**，但**没必要升**：

- 3.6.1 / 3.6.2 修的都是 TLS 证书校验、`pk_write_key_*`、`ecdsa_der_to_raw`
  这类接口的漏洞，本项目**一个都没用到**（只用了 HKDF / ChaCha20-Poly1305 /
  HMAC / 大数运算 / DRBG）
- 3.6.2 的 CMakeLists 会**强制要求 `framework` 子模块**存在，少一个就直接
  配置失败 —— 对国内网络是额外的负担
- 擅自改上游的 pin 会跟上游更新脱节

### 4. 首次构建会拉 Mbed TLS（约 190MB）

由上游的 `FetchContent` 自动下载。网络不好时会卡很久（实测直连 GitHub
只有约 50KB/s，且会中途断开重来）。

**绕过的办法**：手动准备一份源码放进 `build/_deps/mbedtls-src/`，配置时加
`-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<绝对路径>`，这样它就不会联网拉取。

注意两点：

- 只复制 `mbedtls-src` 不够，**`framework/` 子目录也要在**，否则 CMake 配置报
  `framework/CMakeLists.txt not found`
- 复制大目录时 `cp -r` 在 Windows 上可能因为文件锁**静默复制不全**
  （曾出现源 169 个文件、目标 0 个文件）。用 `tar` 管道更可靠：
  `(cd "$SRC" && tar cf - .) | (cd "$DST" && tar xf -)`

### 5. 生成器名字要对上本机

README 的编译命令写的是 `-G "Visual Studio 17 2022"`，但**以本机实际安装为准**：

```bash
cmake --help | grep -i "visual studio"
```

装的是 VS 18 就用 `-G "Visual Studio 18 2026"`，否则会报
`could not find any instance of Visual Studio`。

### 6. 改完上游代码后，patch 要重新生成

补丁是 `third_party/airplay2-sender-cpp/` 相对上游 `fc91350` 的 `git diff`。
改了上游文件后：

```bash
cd third_party/airplay2-sender-cpp
git diff > ../../patches/upstream-airplay2-sender-cpp.patch
```

**两个容易犯的错**：

- 忘了 `git add` / `commit` 就直接去 `clone` 验证 —— clone 只认**已提交**的内容，
  没提交的改动不会出现在新 clone 里，验证会白做
- 新 patch 必须包含**所有**改过的文件。当前含 4 个：
  `CMakeLists.txt`、`src/raop_sender.cpp`、`src/raop_sender.h`、`test/core_tests.cpp`

### 7. 验收标准

改完照这个走一遍，全绿才算完成：

```bash
# 干净克隆 + 子模块
git clone --recursive <repo> && cd <repo>

# 打补丁
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

# 编译
cmake -S . -B build -G "<你本机的生成器>" -A x64
cmake --build build --config Release

# 跑上游测试（231 项，必须 100% 通过）
cd third_party/airplay2-sender-cpp
cmake -S . -B tbuild -G "<你本机的生成器>" -A x64 -DAIRPLAY_BUILD_TESTS=ON
cmake --build tbuild --config Release
ctest --test-dir tbuild -C Release
```

