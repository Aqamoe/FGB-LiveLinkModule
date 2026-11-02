#include "livelink_protocol.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace fgb::kit;
using livelink::Packet;
using livelink::ParsePacket;

namespace {

constexpr std::size_t Slot(Arkit name) { return static_cast<std::size_t>(name); }

// 包尾 61 个 float 的下标，顺序照抄上游 Constants.LiveLinkNames。
constexpr std::size_t kEyeBlinkLeft = 0;
constexpr std::size_t kEyeWideLeft = 6;
constexpr std::size_t kEyeBlinkRight = 7;
constexpr std::size_t kEyeLookOutRight = 10;
constexpr std::size_t kEyeLookUpRight = 11;
constexpr std::size_t kEyeSquintLeft = 5;
constexpr std::size_t kJawOpen = 17;
constexpr std::size_t kMouthSmileLeft = 23;
constexpr std::size_t kTongueOut = 51;
constexpr std::size_t kHeadYaw = 52;
constexpr std::size_t kHeadPitch = 53;
constexpr std::size_t kHeadRoll = 54;
constexpr std::size_t kEyeYawLeft = 55;
constexpr std::size_t kEyePitchLeft = 56;
constexpr std::size_t kEyeYawRight = 58;

std::vector<float> Tail() { return std::vector<float>(livelink::kFloatCount, 0.0f); }

float NaN32() {
    const std::uint32_t bits = 0x7FC00000u;
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// 把 61 个 float 按大端拼成包尾，并在前面加上变长的垃圾头部（设备名等的替身）。
std::vector<std::uint8_t> BuildPacket(const std::vector<float>& floats, std::size_t prefix_bytes) {
    std::vector<std::uint8_t> bytes(prefix_bytes, 0xAA);
    for (const float value : floats) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        bytes.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>(bits & 0xFFu));
    }
    return bytes;
}

FgbFrameSlot FreshSlot() {
    FgbFrameSlot slot{};
    slot.struct_size = sizeof(FgbFrameSlot);
    slot.abi_version = FGB_MODULE_ABI_V2;
    slot.shape_count = FGB_MAX_SHAPES;
    return slot;
}

float VectorLength(const float (&direction)[3]) {
    return std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                     direction[2] * direction[2]);
}

} // namespace

int main() {
    // ---- 正常包：变长头部 + 244 字节尾，大端解析 ----
    std::vector<float> floats = Tail();
    floats[kEyeBlinkLeft] = 0.25f;
    floats[kEyeSquintLeft] = 0.5f; // 存在但不参与睁眼度（include_squint 为 false）
    floats[kEyeBlinkRight] = 0.5f;
    floats[kEyeWideLeft] = 0.2f;
    floats[kJawOpen] = 0.62f;
    floats[kMouthSmileLeft] = 0.4f;
    floats[kTongueOut] = 0.3f;
    floats[kHeadYaw] = 30.0f;
    floats[kHeadPitch] = -20.0f;
    floats[kHeadRoll] = 5.0f;
    floats[kEyeYawLeft] = 12.0f;
    floats[kEyePitchLeft] = -5.0f; // 上游 Gaze.y 取反后应为 +5

    Packet packet{};
    std::string error;
    const std::vector<std::uint8_t> bytes = BuildPacket(floats, 37); // 37 字节垃圾头
    CHECK_EQ(bytes.size(), static_cast<std::size_t>(37 + 244));
    CHECK(ParsePacket(bytes, packet, error));
    CHECK_EQ(packet.mapped, static_cast<std::size_t>(livelink::kExpressionCount));
    CHECK_EQ(packet.nonfinite, static_cast<std::size_t>(0));
    // 大端解码 + 名字归一：值必须落到正确的 ARKit 槽
    CHECK_NEAR(packet.weights[Slot(Arkit::EyeBlinkLeft)], 0.25, 1e-6);
    CHECK_NEAR(packet.weights[Slot(Arkit::EyeBlinkRight)], 0.5, 1e-6);
    CHECK_NEAR(packet.weights[Slot(Arkit::EyeWideLeft)], 0.2, 1e-6);
    CHECK_NEAR(packet.weights[Slot(Arkit::JawOpen)], 0.62, 1e-6);
    CHECK_NEAR(packet.weights[Slot(Arkit::MouthSmileLeft)], 0.4, 1e-6);
    CHECK_NEAR(packet.weights[Slot(Arkit::TongueOut)], 0.3, 1e-6);
    // 头姿与眼动角度不进表情表，单独存 Packet
    CHECK_NEAR(packet.head.yaw_degrees, 30.0, 1e-6);
    CHECK_NEAR(packet.head.pitch_degrees, -20.0, 1e-6);
    CHECK_NEAR(packet.head.roll_degrees, 5.0, 1e-6);
    CHECK_NEAR(packet.left.yaw_degrees, 12.0, 1e-6);
    CHECK_NEAR(packet.left.pitch_degrees, 5.0, 1e-6); // -EyePitch 的上游口径
    CHECK(packet.left.HasMotion());
    CHECK(!packet.right.HasMotion());

    // ---- 头部长度不影响：0 字节头的裸 244 包同样解析 ----
    Packet bare{};
    const std::vector<std::uint8_t> bare_bytes = BuildPacket(floats, 0);
    CHECK(ParsePacket(bare_bytes, bare, error));
    CHECK_NEAR(bare.weights[Slot(Arkit::JawOpen)], 0.62, 1e-6);
    CHECK_NEAR(bare.left.yaw_degrees, 12.0, 1e-6);

    // ---- 小于 244 字节必须整包拒绝 ----
    Packet tiny{};
    const std::vector<std::uint8_t> short_bytes(243, 0x00);
    CHECK(!ParsePacket(short_bytes, tiny, error));
    CHECK(!error.empty());
    const std::vector<std::uint8_t> empty_bytes;
    CHECK(!ParsePacket(empty_bytes, tiny, error));

    // ---- 全 NaN 包：不崩溃、值全部归 0、异常值被计数 ----
    Packet nan_packet{};
    std::vector<float> nan_floats = Tail();
    for (std::size_t i = 0; i < livelink::kFloatCount; ++i) {
        nan_floats[i] = NaN32();
    }
    CHECK(ParsePacket(BuildPacket(nan_floats, 8), nan_packet, error));
    CHECK_EQ(nan_packet.nonfinite, static_cast<std::size_t>(livelink::kFloatCount));
    for (std::size_t i = 0; i < kArkitCount; ++i) {
        CHECK_NEAR(nan_packet.weights[i], 0.0, 1e-6);
    }
    CHECK_NEAR(nan_packet.head.yaw_degrees, 0.0, 1e-6);
    CHECK(!nan_packet.left.HasMotion());

    // ---- 帧写入：通用映射 + 眼睛口径 + 头姿不输出 ----
    testing::HostMock host;
    Host logger{host.services()};
    ShapeTable table;
    livelink::DeclareTargets(table);
    CHECK(table.Build(logger));
    CHECK_EQ(table.ResolvedCount(), table.Size()); // 目标名必须都是真实形状

    FgbFrameSlot slot = FreshSlot();
    livelink::WriteFrame(packet, table, &slot);

    CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
    CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
    CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) != 0);
    CHECK((slot.attributes & FGB_ATTR_HEAD) == 0); // 头姿本期不写帧
    for (const float component : slot.head_rotation) {
        CHECK_NEAR(component, 0.0, 1e-6);
    }

    CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.62, 1e-6);
    CHECK_NEAR(slot.shapes[host.IndexOf("TongueOut")], 0.3, 1e-6);
    CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullLeft")], 0.4, 1e-6);
    CHECK_NEAR(slot.shapes[host.IndexOf("EyeWideLeft")], 0.2, 1e-6);
    CHECK(slot.shape_valid[host.IndexOf("JawLeft")] == 0); // 本包没有 jawLeft

    // 睁眼度只看 blink（与 VRCFT 的 blink 口径一致），squint 不参与
    CHECK_NEAR(slot.left_eye.openness, 0.75, 1e-6);
    CHECK_NEAR(slot.right_eye.openness, 0.5, 1e-6);

    // 左眼有真实眼动角度：gaze_dir 被覆盖，且仍是单位向量、-Z 前
    const float tan_yaw = std::tan(12.0f * 0.0174532924f);
    const float tan_pitch = std::tan(5.0f * 0.0174532924f);
    const float normalize = std::sqrt(tan_yaw * tan_yaw + tan_pitch * tan_pitch + 1.0f);
    CHECK_NEAR(VectorLength(slot.left_eye.gaze_dir), 1.0, 1e-5);
    CHECK_NEAR(slot.left_eye.gaze_dir[0], tan_yaw / normalize, 1e-5);
    CHECK(slot.left_eye.gaze_dir[0] > 0.0f);  // yaw 为正 → 向右
    CHECK(slot.left_eye.gaze_dir[1] > 0.0f);  // pitch 取反后为正 → 向上
    CHECK(slot.left_eye.gaze_dir[2] < 0.0f);
    CHECK_NEAR(slot.left_eye.gaze_confidence, 0.85, 1e-6);

    // 右眼没有真实角度：保留表情系数推导的近似方向（全 0 → 正前方）
    CHECK_NEAR(VectorLength(slot.right_eye.gaze_dir), 1.0, 1e-5);
    CHECK_NEAR(slot.right_eye.gaze_dir[0], 0.0, 1e-6);
    CHECK_NEAR(slot.right_eye.gaze_dir[1], 0.0, 1e-6);
    CHECK_NEAR(slot.right_eye.gaze_dir[2], -1.0, 1e-6);
    CHECK_NEAR(slot.right_eye.gaze_confidence, 0.6, 1e-6);

    // ---- EyeYaw 全零但 eyeLook* 非零：近似方向由系数推导 ----
    Packet approximate{};
    std::vector<float> look_floats = Tail();
    look_floats[kEyeLookUpRight] = 0.4f;
    look_floats[kEyeLookOutRight] = 0.3f;
    CHECK(ParsePacket(BuildPacket(look_floats, 16), approximate, error));
    FgbFrameSlot approx_slot = FreshSlot();
    livelink::WriteFrame(approximate, table, &approx_slot);
    CHECK_NEAR(VectorLength(approx_slot.right_eye.gaze_dir), 1.0, 1e-5);
    CHECK(approx_slot.right_eye.gaze_dir[0] > 0.0f); // lookOutRight → +x
    CHECK(approx_slot.right_eye.gaze_dir[1] > 0.0f); // lookUpRight → +y
    CHECK_NEAR(approx_slot.right_eye.gaze_confidence, 0.6, 1e-6);
    CHECK_NEAR(approx_slot.left_eye.gaze_dir[2], -1.0, 1e-6); // 无数据 → 正前方

    // ---- 全 NaN 包写帧也不崩溃、形状全不置位 ----
    FgbFrameSlot nan_slot = FreshSlot();
    livelink::WriteFrame(nan_packet, table, &nan_slot);
    CHECK_NEAR(nan_slot.left_eye.openness, 1.0, 1e-6);
    CHECK(nan_slot.shape_valid[host.IndexOf("JawOpen")] == 0);

    return CheckSummary("livelink-protocol");
}
