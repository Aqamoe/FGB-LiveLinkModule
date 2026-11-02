# 更新日志

遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [未发布]

## [0.1.0] - 未发布
### 新增
- LiveLink Face（iPhone → PC，UDP 11111）尾包 244 字节 / 61 个大端 float32 →
  Unified Expressions 表情、睁眼度与逐眼注视方向。
- 52 项 ARKit 系数按 Kit 通用表映射；EyeYaw/EyePitch/EyeRoll（度数）有值时
  用真实眼动角度覆盖注视方向（置信度 0.85），否则保留表情近似方向（0.6）。
- 头姿（HeadYaw/Pitch/Roll）解析并保留在 Packet 内，本期不写入帧。
- `config.ini` 支持覆盖端口。
- 协议单测 + 通过真实 ABI 的端到端冒烟测试。

### 已知限制
- 未在真机（iPhone LiveLink Face App）验证，状态为未验证。
- 头姿坐标系与单位未确认，不提供头部旋转输出。
