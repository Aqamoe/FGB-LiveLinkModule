# 真机验收清单

> 当前状态：**未验证**。下列任一项完成后，请在 issue 里附设备与版本信息。

## 环境

| 项 | 值 |
|---|---|
| 手机 App | iPhone 上的 Unreal LiveLink Face App（记录 App 版本与 iOS 版本） |
| PC 软件 | LiveLink Hub / Unreal Engine（记录 UE 版本，若需要中转） |
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（记录版本号） |
| 网络 | 手机与 PC 在同一网段 |

## 步骤

- [ ] iPhone LiveLink Face App 填 PC 的局域网 IP，端口默认 11111
- [ ] PC 上关闭 VRCFT（其 LiveLink 模块抢同一端口）及其它占用 11111 的程序
- [ ] FGB「模块库 → 原生模块」安装 `livelink-<版本>.zip`
- [ ] 「模块」页把 LiveLink 设为启动，重启引擎
- [ ] 模块卡片显示活跃，状态页有表情输入

## 观察项

- [ ] 张嘴 → `JawOpen`
- [ ] 微笑 → 嘴角上扬（`MouthCornerPull*`）
- [ ] 眨眼 → 睁眼度下降，**左右眼独立且没有互换**（左眨眼只降左眼睁眼度）
- [ ] 看左 / 看右 → `gaze_dir` 的 x 分量符号正确（左右确认）
- [ ] 看上 / 看下 → `gaze_dir` 的 y 分量符号正确（上游取反口径 `Gaze.y = -EyePitch` 需人眼确认）
- [ ] 转头 → 表情不跟随（头姿本期不输出），且表情**不**被头部转动污染
- [ ] 伸出舌头 → `TongueOut`
- [ ] 遮挡摄像头 / 离开网段 → 10 秒内卡片给出「已 N 秒没有数据」
- [ ] 恢复推送后自动回到「数据已恢复」并继续出帧（断流恢复）

## 与上游对比（可选但有用）

同一台 iPhone 并用 VRCFT 的 LiveLink 模块，对比同一组表情的参数曲线，
记录明显不一致的形状名。极化（左右眼、yaw/pitch 符号、pitch 取反）特别需要人眼确认。

头姿接入前需要额外确认：`HeadYaw/HeadPitch/HeadRoll` 的单位与符号
（上游写 VRCFT 头姿字段时逐项取反；有说法称还要除以 100，未经真机核实），
以及旋转轴序与坐标系，确认后再决定 `FGB_ATTR_HEAD` 的写法。
