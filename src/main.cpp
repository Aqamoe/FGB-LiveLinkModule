// LiveLink Face（iPhone）→ FaceGaze Bridge 原生模块。
//
// 监听 Unreal LiveLink Face App 发到本机 11111 端口的 UDP 包，取包尾 244 字节
// 解出 61 个大端 float32，把前 52 个 ARKit 系数映射成 Unified Expressions，
// 并给出睁眼度与逐眼方向（有真实眼动角度时优先用它）。
// 不需要 VRCFT，不注入任何进程，也不需要管理员权限。

#include "livelink_protocol.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp"

#include <cstdint>
#include <string>

namespace {

constexpr std::uint16_t kDefaultPort = 11111;
constexpr std::uint64_t kNoDataTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;

class LiveLinkSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        if (config.Load(directory, "config.ini", config_error)) {
            port_ = static_cast<std::uint16_t>(config.GetInt("port", port_));
            if (!config_error.empty()) {
                host.Warn("config.ini：" + config_error);
            }
            if (!config.UnknownKeys().empty()) {
                host.Warn("config.ini 有未识别的键：" + config.UnknownKeys()[0]);
            }
        }

        fgb::kit::ShapeTable table;
        livelink::DeclareTargets(table);
        if (!table.Build(host)) {
            error = "宿主没有提供 shape_index，无法映射 Unified Expressions";
            return false;
        }
        if (table.ResolvedCount() < table.Size()) {
            host.Warn("部分 Unified Expressions 形状未被宿主识别，已跳过 " +
                      std::to_string(table.Size() - table.ResolvedCount()) + " 个");
        }
        table_ = std::move(table);

        if (!endpoint_.Bind(port_, error)) {
            return false;
        }
        host.Info("LiveLink 监听 UDP " + std::to_string(port_) +
                  "，等待 iPhone 上的 LiveLink Face App 推送");
        host.StatusStarting("等待 LiveLink 数据");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        livelink::Packet packet;
        bool parsed = false;

        fgb::kit::UdpPacket datagram;
        // App 按帧率推送，只处理最新一包；积压的旧包丢掉比逐帧回放更准。
        if (endpoint_.PollLatest(datagram)) {
            std::string error;
            if (livelink::ParsePacket(datagram.data, packet, error)) {
                parsed = true;
                // 异常字段限频/去重告警，不刷屏。
                if (packet.nonfinite > 0 && packet.nonfinite != reported_nonfinite_) {
                    reported_nonfinite_ = packet.nonfinite;
                    host.Warn("LiveLink 包里有 " + std::to_string(packet.nonfinite) +
                              " 个 NaN/Inf 数值，已按 0 处理");
                }
            } else {
                host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs, "丢弃一包：" + error);
            }
        }

        if (!staleness_.Check(host, endpoint_.LastPacketNs(), now_ns) || !parsed) {
            return false;
        }

        livelink::WriteFrame(packet, table_, &slot);
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        endpoint_.Close();
        host.StatusStopped("LiveLink 已停止");
    }

private:
    fgb::kit::UdpEndpoint endpoint_;
    fgb::kit::ShapeTable table_;
    fgb::kit::Staleness staleness_{kNoDataTimeoutNs, "LiveLink"};
    std::uint16_t port_ = kDefaultPort;
    std::uint64_t warn_ns_ = 0;
    std::size_t reported_nonfinite_ = 0;
};

} // namespace

FGB_KIT_EXPORT(LiveLinkSource, "com.fgbbridge.livelink", "LiveLink",
               FGB_CAP_EXPRESSION | FGB_CAP_EYES)
