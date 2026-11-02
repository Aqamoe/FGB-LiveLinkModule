// 走真实 ABI 的端到端冒烟：加载 DLL → query → initialize → 喂合成 UDP → update → 检查提交帧。
//
// 它会绑定 11111 端口，运行前关掉 VRCFT（含其 LiveLink 模块）和其它接收端。
// 因此不挂进 ctest，只由 `cmake --build ... --target smoke-test` 触发。

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "fgb_module_api.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kPort = 11111;

// 合成包尾 61 个 float 的下标（顺序 = 上游 LiveLinkNames）。
constexpr std::size_t kEyeBlinkLeft = 0;
constexpr std::size_t kJawOpen = 17;
constexpr std::size_t kMouthSmileLeft = 23;
constexpr std::size_t kEyeYawLeft = 55;

// 变长垃圾头（设备名替身）+ 61 个大端 float32。
std::vector<std::uint8_t> BuildPacket() {
    std::vector<float> floats(61, 0.0f);
    floats[kEyeBlinkLeft] = 0.25f;
    floats[kJawOpen] = 0.62f;
    floats[kMouthSmileLeft] = 0.4f;
    floats[kEyeYawLeft] = 10.0f;

    std::vector<std::uint8_t> bytes(20, 0xAA);
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

bool SendToSelf(const std::vector<std::uint8_t>& bytes) {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    const SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kPort);
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int sent = ::sendto(handle, reinterpret_cast<const char*>(bytes.data()),
                              static_cast<int>(bytes.size()), 0,
                              reinterpret_cast<sockaddr*>(&target), sizeof(target));
    ::closesocket(handle);
    ::WSACleanup();
    return sent != SOCKET_ERROR;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <livelink.dll 路径>\n", argv[0]);
        return 2;
    }

    HMODULE library = ::LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败，错误码 %lu（DLL 是否已构建？依赖是否在同目录？）\n",
                    ::GetLastError());
        return CheckSummary("livelink-abi-smoke");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    fgb::kit::testing::HostMock host;
    FgbInitParams params = host.MakeParams();

    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module == nullptr) {
        ::FreeLibrary(library);
        return CheckSummary("livelink-abi-smoke");
    }
    CHECK(module->id == std::string{"com.fgbbridge.livelink"});
    CHECK_EQ(module->abi_version, FGB_MODULE_ABI_V2);
    CHECK_EQ(module->capabilities,
             static_cast<std::uint32_t>(FGB_CAP_EXPRESSION | FGB_CAP_EYES));
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr); // ABI 不匹配必须拒绝

    void* self = nullptr;
    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    if (self == nullptr) {
        std::printf("initialize 失败：%s\n", host.LastStatusDetail().c_str());
        ::FreeLibrary(library);
        return CheckSummary("livelink-abi-smoke");
    }

    // 没有数据时不应该提交帧
    module->update(self);
    CHECK(host.committed().empty());

    CHECK(SendToSelf(BuildPacket()));
    for (int attempt = 0; attempt < 200 && host.committed().empty(); ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!host.committed().empty());

    if (!host.committed().empty()) {
        const FgbFrameSlot& slot = host.committed().back();
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_HEAD) == 0); // 头姿本期不输出
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.62, 1e-5);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullLeft")], 0.4, 1e-5);
        CHECK_NEAR(slot.left_eye.openness, 0.75, 1e-5);
        // 有真实眼动角度：方向被覆盖成单位向量且带 0.85 置信度
        const float length = std::sqrt(slot.left_eye.gaze_dir[0] * slot.left_eye.gaze_dir[0] +
                                       slot.left_eye.gaze_dir[1] * slot.left_eye.gaze_dir[1] +
                                       slot.left_eye.gaze_dir[2] * slot.left_eye.gaze_dir[2]);
        CHECK_NEAR(length, 1.0, 1e-5);
        CHECK(slot.left_eye.gaze_dir[0] > 0.0f); // EyeYawLeft = +10 度 → 向右
        CHECK(slot.left_eye.gaze_dir[2] < 0.0f);
        CHECK_NEAR(slot.left_eye.gaze_confidence, 0.85, 1e-5);
        CHECK_NEAR(slot.right_eye.gaze_confidence, 0.6, 1e-5); // 无角度 → 近似方向
        CHECK(host.HasStatus(FGB_STATUS_STARTING) || host.HasStatus(FGB_STATUS_ACTIVE));
    }

    module->teardown(self);
    ::FreeLibrary(library);
    return CheckSummary("livelink-abi-smoke");
}
