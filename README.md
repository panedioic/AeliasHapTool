# AeliasHapTool

一个自包含的 **HAP 开发助手**命令行工具：把 OpenHarmony 的 `hdc` 与 C++ 版签名工具编译成共享库，
在同一个进程内提供 **设备连接 / 安装 / 卸载 / 启动 / 签名 / 工程生成** 全套能力，不依赖
Java、Python 或 DevEco Studio。

```
haptool template ./demo com.example.demo     # 生成 C++ (NAPI) 空工程
haptool sign login                            # 浏览器登录华为开发者账号
haptool sign add ./demo/entry/build/.../entry-default-unsigned.hap
haptool hap sign-install ./signed.hap         # 签名并安装
haptool device shell param get const.product.model
```

---

## 为什么是"编译成 so"

上游 `hdc` 与 `hapsigntool_cpp` 都是独立的可执行程序。很多场景（应用沙箱、精简系统镜像、
需要把工具随应用分发的场合）里既没有 `java`，也不方便携带一整套可执行文件。
把它们各自编成 `libhdc.so` / `libhapsigner.so`，再由 `haptool` 链接，只留三个文件就能工作：

```
haptool            主程序（命令层 + 云端签名流程 + 模板生成）
libhdc.so          hdc host 侧（连接/安装/卸载/shell/forward…）
libhapsigner.so    hap-sign-tool（sign-app / verify-app / generate-* / sign-profile…）
```

三者放在同一目录即可运行（主程序 `rpath=$ORIGIN`）。

---

## 功能

| 分组 | 命令 | 说明 |
|---|---|---|
| 设备 | `device list` | 列出已连接设备 |
| | `device connect <ip:port>` | TCP 连接（tconn） |
| | `device shell <cmd...>` | 设备上执行命令 |
| 应用 | `hap install <hap> [device]` | 安装 |
| | `hap uninstall <bundle> [device]` | 卸载 |
| | `hap open <bundle> [ability]` | 启动应用（`aa start`） |
| 签名 | `sign login` / `logout` / `status` | 华为开发者账号登录态 |
| | `sign add [bundle\|hap]` | 按包名申请调试证书 / 注册设备 / 申请 Profile |
| | `sign list` / `info` / `remove` | 材料管理 |
| | `sign import [id]` | 导入手工签名配置（自有 keystore） |
| | `hap sign <hap> [bundle]` | 用本地材料签名 |
| | `hap sign-install <hap>` | 签名后安装 |
| | `hap genkey [alias] [out.p12]` | 生成 ECC P-256 密钥对（不联网） |
| | `hap signer <args...>` | 透传全部 `hap-sign-tool` 子命令 |
| 工程 | `template <path> [bundle] [api]` | 生成 C++ (NAPI) 空 HAP 工程 |
| 交互 | `haptool` | 无参数进入 REPL；带参数执行单条命令后退出（退出码可脚本判断） |

`haptool help` 查看完整列表。

---

## 构建

### 依赖

- Linux 工具链：`gcc/g++`、`make`、`git`、`tar`
- OpenHarmony SDK（DevEco command-line-tools 布局），默认 `/apps/harmony`，可用 `TOOL_HOME` / `OHOS_SDK` 覆盖
- 第三方源码：openssl / libuv / lz4 / libusb / zlib（`build_deps.sh` 自动下载并静态编译）

### 三步构建（宿主 linux）

```bash
./scripts/build_deps.sh  all    # openssl/libuv/lz4/libusb/zlib → build/deps-linux/
./scripts/build_hdc.sh   all    # clone 上游 + 58 个 TU → build/out/linux/libhdc.so
./scripts/build_hapsigner.sh all # clone 上游 + 100 个 TU → build/out/linux/libhapsigner.so
./scripts/build.sh              # src/* + cJSON + minizip → build/out/linux/haptool
```

### 交叉编译到 OpenHarmony（aarch64）

```bash
PLATFORM=ohos ./scripts/build_deps.sh      build
PLATFORM=ohos ./scripts/build_hdc.sh       build
PLATFORM=ohos ./scripts/build_hapsigner.sh build
PLATFORM=ohos ./scripts/build.sh
# 产物：build/out/ohos/{haptool,libhdc.so,libhapsigner.so}
```

OHOS 目标用 SDK clang（`--target=aarch64-linux-ohos`，动态 musl）；`libuv` 直接链 SDK
sysroot 的 `libuv.so`，`libusb` 用 `compat/hdc/libusb_stub.cpp`（设备内无 USB host 场景，
TCP 通道不受影响）。

### 平台与脚本一览

| 脚本 | 阶段 | 作用 |
|---|---|---|
| `scripts/env.sh` | — | 公共变量与函数（工具链探测、补丁、并行编译） |
| `scripts/build_deps.sh` | `src` / `build` / `all` | 第三方静态库 |
| `scripts/build_hdc.sh` | `src` / `build` / `all` | `developtools_hdc` → `libhdc.so` |
| `scripts/build_hapsigner.sh` | `src` / `build` / `all` | `developtools_hapsigner/hapsigntool_cpp` → `libhapsigner.so` |
| `scripts/build.sh` | `tool` / `all` | 主程序 |

所有阶段幂等：`src` 阶段只在缺失时 clone，编译产物按需重建。

---

## 签名材料与 `AHT_HOME`

签名材料集中存放在工作主目录（默认 `$HOME/.ahthome`，可用 `AHT_HOME` 覆盖）：

```
$AHT_HOME/
  auth.json                   登录态（jwt / accessToken / uid / teamId）
  sign.json                   手工导入的签名配置（sign import）
  profiles/<bundle>/
    key.p12                   私钥库（自动生成）
    keystore.pwd              钥库口令（0600）
    cert.cer                  调试证书
    profile.p7b               调试 Profile
    meta.json                 包名 / UDID / 证书 / 到期时间 / keyAlias
```

复用规则：**包名一致 + UDID 集合一致 + Profile 未过期（余量 1 天）** → 直接本地签名，
完全不联网。任一不满足才走云端补齐。

因为所有材料都是普通文件，**整个 `AHT_HOME` 可以拷到另一台设备**，删掉 `auth.json`
也能继续签名（登录态只在申请新材料时需要）。

---

## 架构

```
src/
  main.cpp          命令解析 / 分发 / REPL；fork 子进程调库入口
  aht_http.cpp      HTTPS 客户端（OpenSSL 手写 HTTP/1.1，无 libcurl 依赖）
  aht_cloud.cpp     登录回调服务 + token 交换 + 证书/设备/Profile 接口
  aht_hap.cpp       HAP 元信息（minizip 读 module.json、ACL 权限、签名块检测）
  aht_store.cpp     AHT_HOME 布局与材料复用判定
  aht_sign.cpp      材料准备与本地签名
  aht_template.cpp  工程模板（内嵌在二进制，无外部模板目录）
compat/
  hdc/              宿主侧替身：HdcLibMain 入口、securec、HUKS/libusb/parameter stub、前缀头
  hapsigntool/      HapSignerLibMain 入口、securec、前缀头
patches/            打在上游源码副本上的补丁（上游只读）
thirdparty/         上游 clone（.gitignore）+ 随仓 cJSON / minizip
```

### 为什么每条命令都 fork 子进程

- `hdc` client 模式结尾会 `_exit(0)`，并残留 `getopt` / libuv 全局状态
- 签名工具会携带 OpenSSL 全局状态，失败路径还可能直接退出

`runIsolated()` 把子进程 stdout/stderr 接到管道，父进程（REPL）保持干净，退出码透传。

### 上游零改动

上游仓库只作为只读参考：编译在 `build/` 下的副本上进行，差异全部收敛在 `compat/`
（入口替身 + 最小 stub）与 `patches/`（当前一个补丁：PKCS12 默认 PBE 换成 AES-256）。
上游更新时同步 `compat/*/xxx_entry.cpp` 与 `patches/` 即可。

---

## 已知限制

- **JKS 不支持**：`hap-sign-tool` C++ 版只读 PKCS12。Java 格式的 keystore 先用
  `keytool -importkeystore -deststoretype PKCS12` 转换。
- **老式 RC2/3DES 加密的 p12 不支持**：构建用的 OpenSSL 是 `no-shared` 静态库，没有
  legacy provider 模块。`hap genkey` 产出的和 keytool 转出的都是 AES 格式，不受影响。
- **打包能力未接入**：`app_packing_tool`（把编译产物打成 hap）尚未编入；模板工程请用
  `hvigorw assembleHap` 构建。
- **云端接口是华为给 DevEco 用的内部协议**：随时可能变更。仅用于给自己有权使用的应用
  做本机调试签名。
- **模板默认 API 版本 `6.1.1(24)`**：可在命令里指定；纯数字会按映射表转成
  `platformVersion(api)`，表外数字原样写入（构建可能报 `api version illegal`）。

---

## 许可与致谢

本项目自身的代码用于整合上述上游能力。上游组件遵循各自许可：

- [OpenHarmony developtools_hdc](https://gitcode.com/openharmony/developtools_hdc) — Apache-2.0
- [OpenHarmony developtools_hapsigner](https://gitcode.com/openharmony/developtools_hapsigner) — Apache-2.0
- OpenSSL — Apache-2.0
- libuv / libusb / lz4 / zlib — MIT / LGPL-2.1 / BSD-2-Clause / zlib

登录与云端申请流程参考了 DevEco Studio 的公开调用方式，仅供个人调试签名使用。
