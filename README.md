# FGB LiveLinkModule

[![build](https://github.com/Aqamoe/FGB-LiveLinkModule/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-LiveLinkModule/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。协议解析、映射与 ABI 生命周期有单元测试和合成 UDP
端到端冒烟；真机（iPhone LiveLink Face App）验收待补，见 [docs/verification.md](docs/verification.md)。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 LiveLink 原生模块。
直接接收 **iPhone 上 Unreal LiveLink Face App** 发到本机 11111 端口的 UDP 包，
解析包尾 244 字节的 61 个大端 float32，输出 Unified Expressions 表情、睁眼度与
逐眼注视方向。不需要 VRCFT，不注入任何进程，也不需要管理员权限。

LiveLink (iOS Face App) native input module for FaceGaze Bridge. Reads the
UDP face stream your phone already sends (last 244 bytes = 61 big-endian
float32) and outputs Unified Expressions shapes, eye openness and per-eye
gaze. No VRCFT, no process injection, no admin rights.

## 功能 / Features

- **表情**：52 项 ARKit 标准系数（包尾前 52 个 float）→ Unified Expressions 形状，
  映射走 FGB-Module-Kit 的通用表。
- **眼睛**：睁眼度按 blink 口径 `1 - clamp(eyeBlink)`（与 VRCFT 的 blink 处理一致，
  不叠加 squint）；有 EyeYaw/EyePitch/EyeRoll（度数）时用真实眼动角度覆盖注视方向
  （度数转弧度后取 tan 构单位向量，置信度 0.85），否则由 eyeLook* 系数推近似方向（0.6）。
- **断连可恢复**：10 秒没有数据时在模块卡片提示原因，数据恢复后自动回到活跃。
- **可诊断**：端口冲突、包体过短、NaN/Inf 脏值都有明确日志（限频，不刷屏），不静默丢数据。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 头姿 | 报文里有 `HeadYaw/HeadPitch/HeadRoll`（度），上游写入 VRCFT 头姿字段时逐项取反，但坐标系/单位/符号未经真机确认，先不接（避免写错方向的头姿）；数值已保留在 `Packet` 里供后续接入 |
| 瞳孔 | 报文没有该字段（上游伪造 5mm 常数，本模块不伪造） |
| 2D 注视点 | 报文没有；方向是 iPhone 摄像头估计的角度，不用于 foveation |
| 渲染级注视 | 需要 `gaze` 通道时请配真正的眼动源；本模块只填 `eyes` |

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（原生模块 ABI v2） |
| 手机 | iPhone 安装 Unreal LiveLink 的 Face App，并在 App 里填这台 PC 的局域网 IP |
| 其它 | 关闭 VRCFaceTracking：它的 LiveLink 模块和本模块抢同一个 UDP 11111 端口 |

## 安装

1. 从 Releases 下载 `livelink-0.1.0.zip`。
2. FaceGaze Bridge →「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」。
3. 「模块」页把 **LiveLink** 设为启动，角色通常给 `expression` 与 `eyes`。
4. 重启引擎；iPhone 的 Face App 开始推送后模块卡片应显示活跃。

## 配置

模块目录下可选 `config.ini`：

```ini
port = 11111
```

改完重启引擎，并同步修改手机 App 里的目标端口。未知键会被记录一条警告，不会静默失效。

## 从源码构建

需要 Visual Studio 2022（x64）、CMake ≥ 3.24 与 Node 20（脚本用）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json` 在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。
本地想指向已解压的 Kit：

```powershell
cmake -S . -B build -A x64 -DFGB_KIT_DIR="../FGB-Module-Kit"
```

打包与测试：

```powershell
cmake --build build --config Release --target package-modules   # build/Release/livelink-<版本>.zip
ctest --test-dir build -C Release --output-on-failure           # 协议与映射单测
cmake --build build --config Release --target smoke-test        # 真实 ABI + 合成 UDP 冒烟（需 11111 空闲）
```

## 目录结构

```
src/main.cpp                  ABI 生命周期、UDP 接收、状态上报
src/livelink_protocol.*       尾包解析与 Unified Expressions 写入
module/                       fgb-native.json 与图标
tests/                        协议单测、真实 ABI 冒烟
docs/verification.md          真机验收清单
cmake/                        依赖锁解析（SDK / Kit 都是拉取，不复制）
```

## 协议要点

来源：iPhone LiveLink Face App 向 PC 推送的 UDP 数据报，默认端口 **11111**
（上游 VRCFT 模块同一端口）。本模块按线格式独立实现，未复制上游源码。

- 报文**前面是变长的静态数据**（含手机设备名等），长度不保证；面部数据固定在**包尾最后 244 字节**。
- 244 字节按 4 字节切成 **61 个 float32**，字节序为**大端**（x86 上需要反转）。
- 前 52 项就是 ARKit 标准系数名（大小写与 Left/Right 后缀不同，用 Kit 的 `ArkitFromName` 归一）；
  后 9 项是头姿与逐眼眼动角度（度数）。
- 上游对第 61 个值的处理依赖其名字表长度（社区快照读第 61 个名字越界会整帧丢弃）；
  等价语义是「用前 60 个值，忽略第 61 个」。本模块把 61 个值全部解出：前 52 项进表情表，
  52-54 存头姿（不输出），55-60 存逐眼角度，第 61 个（EyeRollRight）只保留、不参与任何输出，
  **不会**像上游那样丢弃整帧。
- NaN/Inf 一律按 0 处理并记一条去重告警；包体不足 244 字节整包拒绝。

61 个 float 的字段顺序（即上游 `Constants.LiveLinkNames`）：

| # | 字段 | # | 字段 | # | 字段 |
|---|---|---|---|---|---|
| 0 | EyeBlinkLeft | 21 | MouthLeft | 42 | BrowDownRight |
| 1 | EyeLookDownLeft | 22 | MouthRight | 43 | BrowInnerUp |
| 2 | EyeLookInLeft | 23 | MouthSmileLeft | 44 | BrowOuterUpLeft |
| 3 | EyeLookOutLeft | 24 | MouthSmileRight | 45 | BrowOuterUpRight |
| 4 | EyeLookUpLeft | 25 | MouthFrownLeft | 46 | CheekPuff |
| 5 | EyeSquintLeft | 26 | MouthFrownRight | 47 | CheekSquintLeft |
| 6 | EyeWideLeft | 27 | MouthDimpleLeft | 48 | CheekSquintRight |
| 7 | EyeBlinkRight | 28 | MouthDimpleRight | 49 | NoseSneerLeft |
| 8 | EyeLookDownRight | 29 | MouthStretchLeft | 50 | NoseSneerRight |
| 9 | EyeLookInRight | 30 | MouthStretchRight | 51 | TongueOut |
| 10 | EyeLookOutRight | 31 | MouthRollLower | 52 | HeadYaw |
| 11 | EyeLookUpRight | 32 | MouthRollUpper | 53 | HeadPitch |
| 12 | EyeSquintRight | 33 | MouthShrugLower | 54 | HeadRoll |
| 13 | EyeWideRight | 34 | MouthShrugUpper | 55 | EyeYawLeft |
| 14 | JawForward | 35 | MouthPressLeft | 56 | EyePitchLeft |
| 15 | JawLeft | 36 | MouthPressRight | 57 | EyeRollLeft |
| 16 | JawRight | 37 | MouthLowerDownLeft | 58 | EyeYawRight |
| 17 | JawOpen | 38 | MouthLowerDownRight | 59 | EyePitchRight |
| 18 | MouthClose | 39 | MouthUpperUpLeft | 60 | EyeRollRight |
| 19 | MouthFunnel | 40 | MouthUpperUpRight | - | - |
| 20 | MouthPucker | 41 | BrowDownLeft | - | - |

> 0-51（共 52 项）为 ARKit 表情系数，用 Kit 通用表映射到 Unified Expressions；
> 52-54 为头姿角度（解析保留、不输出）；55-60 为逐眼眼动角度（度），有值时覆盖注视方向；
> 第 60 项 `EyeRollRight` 只保留在 Packet 内、不参与任何输出。

## 排错

| 现象 | 处理 |
|---|---|
| 卡片显示「无法绑定 UDP 11111（端口已被占用）」 | 关闭 VRCFT（其 LiveLink 模块同用 11111）或其它接收端；换端口需同步改手机 App 设置 |
| 一直提示「还没收到任何数据包」 | 确认 iPhone 与 PC 同网段、App 里填的是 PC 地址、防火墙放行 UDP 11111 |
| 表情不动、眼睛正常 | 角色没分给 `expression` |
| 左右眼数据疑似互换 | 极化按上游 `Gaze.x = EyeYaw`、`Gaze.y = -EyePitch` 实现，未经真机确认；请按 docs/verification.md 核对后反馈 |
| 日志出现「NaN/Inf 数值，已按 0 处理」 | 上游 App 发了脏值；把 iOS/App 版本号和日志一并提到 issue |
| 头部转动没有输出 | 属正常：本模块不接头姿，见「不提供」 |

## 致谢 / Attribution

- 线格式与映射依据参考 [VRCFaceTracking/LiveLinkTrackingModule](https://github.com/VRCFaceTracking/LiveLinkTrackingModule)；
  该仓库未声明许可证，本仓库是同一线格式的**独立 C++ 实现，未复制其源码**。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。
- Unreal Engine、LiveLink 为 Epic Games, Inc. 的商标；本仓库与其无关联。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
