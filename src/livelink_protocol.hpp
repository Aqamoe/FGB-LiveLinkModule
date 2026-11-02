#pragma once
// LiveLink Face（iPhone → PC:11111）UDP 尾包解析与帧写入。
//
// 上游格式（Unreal LiveLink 的 iOS Face App 推到 PC）：
//   报文前面是变长的静态数据（含手机设备名等，长度不保证），
//   包尾最后 244 字节才是面部数据，按 4 字节切成 61 个**大端** float32，
//   顺序对应上游 Constants.LiveLinkNames（前 52 项是 ARKit 标准系数名，
//   52-54 是头姿角度，55-60 是逐眼真实眼动角度，均为度）。
// 上游读第 61 个名字时会因名字表越界抛异常并整帧丢弃；等价语义是
// 「用前 60 个值，忽略第 61 个」——这里第 61 个（EyeRollRight）只保留在
// Packet 里，不参与任何输出，也不会让整帧失效。

#include "fgb/kit/arkit.hpp"
#include "fgb/kit/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace livelink {

constexpr std::size_t kTailBytes = 244;    // 面部数据的固定长度
constexpr std::size_t kFloatCount = 61;    // 244 / 4
constexpr std::size_t kExpressionCount = 52; // 前 52 项是 ARKit 表情系数

struct HeadPose {
    // HeadYaw/HeadPitch/HeadRoll 按度上报；上游写进 VRCFT 头姿字段时逐项取反。
    // 坐标系与单位未真机确认，本期只保留数值、不写进帧，供后续接入。
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    float roll_degrees = 0.0f;
};

struct EyeAngles {
    // EyeYaw/EyePitch/EyeRoll 是度数的真实眼动角度，ARKit 系数表里没有对应项。
    // 上游口径：Gaze.x = EyeYaw、Gaze.y = -EyePitch，pitch 在这里就取反存好。
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    float roll_degrees = 0.0f;

    // 三个角都为 0 视为「本包没有眼动数据」，保留表情系数推导的近似方向。
    bool HasMotion() const;
};

struct Packet {
    fgb::kit::ArkitWeights weights{};
    HeadPose head{};
    EyeAngles left{};
    EyeAngles right{};
    std::size_t mapped = 0;    // 成功进入 ARKit 系数表的字段数（期望恒为 52）
    std::size_t nonfinite = 0; // 被清洗成 0 的 NaN/Inf 值数量
};

// 解析一包原始字节。返回 false 表示这一包不可信（长度不够），调用方应整包丢弃。
// 解析只消费包尾 244 字节，前面的变长头部不影响结果。
bool ParsePacket(const std::uint8_t* data, std::size_t size, Packet& out, std::string& error);
bool ParsePacket(std::string_view text, Packet& out, std::string& error);
bool ParsePacket(const std::vector<std::uint8_t>& bytes, Packet& out, std::string& error);

// 登记本模块会写的全部 Unified Expressions 目标名。
void DeclareTargets(fgb::kit::ShapeTable& table);

// 写表情 + 双眼属性，并置上对应的 attributes 位。头姿本期不写入。
void WriteFrame(const Packet& packet, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot);

} // namespace livelink
