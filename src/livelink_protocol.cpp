#include "livelink_protocol.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace livelink {

using fgb::kit::ArkitWeights;
using fgb::kit::ShapeTable;

namespace {

constexpr float kDegreeToRadian = 0.0174532924f;

// 包尾 61 个 float 的字段名，顺序与上游 Constants.LiveLinkNames 一一对应。
// 前 52 项是 ARKit 标准名（大小写不同、Left/Right 后缀），交给
// fgb::kit::SetArkitByName 归一；后 9 项不是表情系数，单独存放。
constexpr const char* kFieldNames[kFloatCount] = {
    "EyeBlinkLeft",     "EyeLookDownLeft",   "EyeLookInLeft",      "EyeLookOutLeft",
    "EyeLookUpLeft",    "EyeSquintLeft",     "EyeWideLeft",        "EyeBlinkRight",
    "EyeLookDownRight", "EyeLookInRight",    "EyeLookOutRight",    "EyeLookUpRight",
    "EyeSquintRight",   "EyeWideRight",      "JawForward",         "JawLeft",
    "JawRight",         "JawOpen",           "MouthClose",         "MouthFunnel",
    "MouthPucker",      "MouthLeft",         "MouthRight",         "MouthSmileLeft",
    "MouthSmileRight",  "MouthFrownLeft",    "MouthFrownRight",    "MouthDimpleLeft",
    "MouthDimpleRight", "MouthStretchLeft",  "MouthStretchRight",  "MouthRollLower",
    "MouthRollUpper",   "MouthShrugLower",   "MouthShrugUpper",    "MouthPressLeft",
    "MouthPressRight",  "MouthLowerDownLeft", "MouthLowerDownRight", "MouthUpperUpLeft",
    "MouthUpperUpRight", "BrowDownLeft",     "BrowDownRight",      "BrowInnerUp",
    "BrowOuterUpLeft",  "BrowOuterUpRight",  "CheekPuff",          "CheekSquintLeft",
    "CheekSquintRight", "NoseSneerLeft",     "NoseSneerRight",     "TongueOut",
    "HeadYaw",          "HeadPitch",         "HeadRoll",           "EyeYawLeft",
    "EyePitchLeft",     "EyeRollLeft",       "EyeYawRight",        "EyePitchRight",
    "EyeRollRight",
};

// 尾部的固定槽位（相对 kFieldNames）。用具名常量代替魔法数字，测试也引用它们。
constexpr std::size_t kHeadYawIndex = 52;
constexpr std::size_t kEyeYawLeftIndex = 55;
constexpr std::size_t kEyeYawRightIndex = 58;

// 大端 float32：线上字节序与 x86 相反，先按大端拼出比特再位拷贝成 float。
float ReadBigEndianFloat(const std::uint8_t* bytes) {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        bits = (bits << 8u) | bytes[i];
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// 头姿与眼动角度的清洗：NaN/Inf 当 0，但保留符号与「度数」量级（不裁到 [0,1]）。
float CleanAngle(float value) {
    if (std::isnan(value) || std::isinf(value)) {
        return 0.0f;
    }
    // 眼动角超过 ±90 度没有物理意义，截断以免 tan 爆炸。
    if (value > 90.0f) {
        return 90.0f;
    }
    if (value < -90.0f) {
        return -90.0f;
    }
    return value;
}

void ReadEyeAngles(const float* floats, EyeAngles& out) {
    out.yaw_degrees = CleanAngle(floats[0]);
    // 与上游一致：pitch 取反后再进帧。
    out.pitch_degrees = -CleanAngle(floats[1]);
    out.roll_degrees = CleanAngle(floats[2]);
}

// 上游给的是角度（度），ABI 要的是 HMD 局部单位向量（-Z 前）。
// 用 tan 把偏航/俯仰转成分量，等效于把角度当作投影角处理，与 MeowFace 同口径。
void GazeFromAngles(float yaw_degrees, float pitch_degrees, float (&direction)[3]) {
    const float x = std::tan(yaw_degrees * kDegreeToRadian);
    const float y = std::tan(pitch_degrees * kDegreeToRadian);
    const float length = std::sqrt(x * x + y * y + 1.0f);
    direction[0] = x / length;
    direction[1] = y / length;
    direction[2] = -1.0f / length;
}

} // namespace

bool EyeAngles::HasMotion() const {
    return yaw_degrees != 0.0f || pitch_degrees != 0.0f || roll_degrees != 0.0f;
}

bool ParsePacket(const std::uint8_t* data, std::size_t size, Packet& out, std::string& error) {
    out = Packet{};
    if (data == nullptr) {
        error = "空数据包";
        return false;
    }
    if (size < kTailBytes) {
        error = "包体小于 " + std::to_string(kTailBytes) + " 字节（收到 " + std::to_string(size) +
                "），不是 LiveLink 面部包";
        return false;
    }

    // 包头是变长静态数据（设备名等），只取包尾 244 字节按大端解出 61 个 float。
    const std::uint8_t* tail = data + size - kTailBytes;
    float floats[kFloatCount];
    for (std::size_t i = 0; i < kFloatCount; ++i) {
        floats[i] = ReadBigEndianFloat(tail + i * 4);
    }

    // 前 52 项是 ARKit 表情系数；名字归一与 NaN/Inf 清洗都在 SetArkitByName 里做。
    for (std::size_t i = 0; i < kExpressionCount; ++i) {
        if (fgb::kit::SetArkitByName(out.weights, kFieldNames[i], floats[i])) {
            ++out.mapped;
        }
        if (std::isnan(floats[i]) || std::isinf(floats[i])) {
            ++out.nonfinite;
        }
    }

    // 头姿三个角度单独存放（本期不输出）；上游读第 61 个名字越界会丢整帧，
    // 这里等价地只用前 60 个值——第 61 个（EyeRollRight）仍然解出并保留。
    out.head.yaw_degrees = CleanAngle(floats[kHeadYawIndex]);
    out.head.pitch_degrees = CleanAngle(floats[kHeadYawIndex + 1]);
    out.head.roll_degrees = CleanAngle(floats[kHeadYawIndex + 2]);
    ReadEyeAngles(floats + kEyeYawLeftIndex, out.left);
    ReadEyeAngles(floats + kEyeYawRightIndex, out.right);
    for (std::size_t i = kExpressionCount; i < kFloatCount; ++i) {
        if (std::isnan(floats[i]) || std::isinf(floats[i])) {
            ++out.nonfinite;
        }
    }

    if (out.mapped != kExpressionCount) {
        // 名字表与 Kit 的 ARKit 表不一致（上游改名了）；能写多少写多少，但要报出来。
        error = "字段名表异常：只识别出 " + std::to_string(out.mapped) + "/" +
                std::to_string(kExpressionCount) + " 个表情系数";
        return false;
    }
    return true;
}

bool ParsePacket(std::string_view text, Packet& out, std::string& error) {
    return ParsePacket(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), out, error);
}

bool ParsePacket(const std::vector<std::uint8_t>& bytes, Packet& out, std::string& error) {
    return ParsePacket(bytes.data(), bytes.size(), out, error);
}

void DeclareTargets(ShapeTable& table) {
    fgb::kit::DeclareUnifiedTargets(table);
}

void WriteFrame(const Packet& packet, const ShapeTable& table, FgbFrameSlot* slot) {
    slot->attributes |= FGB_ATTR_EXPRESSION;
    fgb::kit::MapArkitToUnified(packet.weights, table, slot);

    // 睁眼与近似方向先由 Kit 从表情系数推一遍（blink 口径，不含 squint），
    // 随后若本包带真实眼动角度再覆盖 gaze_dir。
    fgb::kit::ArkitEyeOptions options;
    options.include_squint = false;
    options.gaze_confidence = 0.6f;
    slot->attributes |= fgb::kit::ApplyArkitEyes(packet.weights, slot, options);

    if (packet.left.HasMotion()) {
        GazeFromAngles(packet.left.yaw_degrees, packet.left.pitch_degrees,
                       slot->left_eye.gaze_dir);
        // 方向来自 LiveLink 的眼动角度估计，置信度高于表情近似、低于设备级追踪。
        slot->left_eye.gaze_confidence = 0.85f;
    }
    if (packet.right.HasMotion()) {
        GazeFromAngles(packet.right.yaw_degrees, packet.right.pitch_degrees,
                       slot->right_eye.gaze_dir);
        slot->right_eye.gaze_confidence = 0.85f;
    }
    // 头姿本期不写入：坐标系与单位未真机确认，见 README「不提供」。
}

} // namespace livelink
