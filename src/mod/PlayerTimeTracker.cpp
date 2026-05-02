#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/data/KeyValueDB.h"
#include "ll/api/io/Logger.h"
#include "ll/api/io/FileUtils.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "mc/world/actor/player/Player.h"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <memory>

namespace playertime {

// ======================== 数据结构 ========================
struct PlayerRecord {
    uint64_t totalSeconds; // 累计在线秒数
};

// ======================== 主类 ========================
class PlayerTimeTracker {
public:
    static PlayerTimeTracker& getInstance() {
        static PlayerTimeTracker instance;
        return instance;
    }

    PlayerTimeTracker() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    // ======================== 生命周期 ========================
    bool load() {
        auto& logger = getSelf().getLogger();
        logger.info("PlayerTimeTracker loading...");

        // 初始化 KeyValueDB
        auto dbPath = getSelf().getDataDir() / "playtime_db";
        db = std::make_unique<ll::data::KeyValueDB>(dbPath);
        logger.info("Database opened at: {}", dbPath.string());
        return true;
    }

    bool enable() {
        auto& logger = getSelf().getLogger();
        logger.info("PlayerTimeTracker enabling...");
        registerEvents();
        return true;
    }

    bool disable() {
        auto& logger = getSelf().getLogger();
        logger.info("PlayerTimeTracker disabling...");
        unregisterEvents();
        // 结算所有在线玩家
        flushAllOnline();
        return true;
    }

private:
    // ======================== 事件监听 ========================
    ll::event::ListenerPtr joinListener;
    ll::event::ListenerPtr disconnectListener;

    void registerEvents() {
        auto& bus = ll::event::EventBus::getInstance();

        joinListener = bus.emplaceListener<ll::event::PlayerJoinEvent>(
            [this](ll::event::PlayerJoinEvent& event) {
                auto& player = event.self();
                auto uuid = player.getUuid().asString();
                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count();

                {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    joinTimes[uuid] = timestamp;
                }

                getSelf().getLogger().info(
                    "[PlayerTimeTracker] Player joined: {} (UUID: {}) at timestamp {}",
                    player.getRealName(), uuid, timestamp);
            }
        );

        disconnectListener = bus.emplaceListener<ll::event::PlayerDisconnectEvent>(
            [this](ll::event::PlayerDisconnectEvent& event) {
                auto& player = event.self();
                auto uuid = player.getUuid().asString();
                auto now = std::chrono::system_clock::now();
                auto currentTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count();

                uint64_t joinTimestamp = 0;
                {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    auto it = joinTimes.find(uuid);
                    if (it != joinTimes.end()) {
                        joinTimestamp = it->second;
                        joinTimes.erase(it);
                    } else {
                        joinTimestamp = currentTimestamp; // 防异常：按 0 秒处理
                    }
                }

                uint64_t sessionSeconds = 0;
                if (currentTimestamp > joinTimestamp) {
                    sessionSeconds = currentTimestamp - joinTimestamp;
                }

                uint64_t totalSeconds = 0;
                {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    auto oldVal = db->get(uuid);
                    if (oldVal) {
                        totalSeconds = std::stoull(*oldVal);
                    }
                    totalSeconds += sessionSeconds;
                    db->set(uuid, std::to_string(totalSeconds));
                }

                getSelf().getLogger().info(
                    "[PlayerTimeTracker] Player left: {} (UUID: {}), "
                    "session: {}s, total: {}s",
                    player.getRealName(), uuid, sessionSeconds, totalSeconds);
            }
        );
    }

    void unregisterEvents() {
        auto& bus = ll::event::EventBus::getInstance();
        bus.removeListener(joinListener);
        bus.removeListener(disconnectListener);
    }

    // ======================== 数据持久化 ========================
    std::unique_ptr<ll::data::KeyValueDB> db;
    std::unordered_map<std::string, uint64_t> joinTimes; // uuid → join_timestamp(秒)
    std::mutex dbMutex;

    // 插件卸载时结算所有在线玩家
    void flushAllOnline() {
        auto now = std::chrono::system_clock::now();
        auto currentTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();

        std::lock_guard<std::mutex> lock(dbMutex);
        for (auto& [uuid, joinTimestamp] : joinTimes) {
            uint64_t sessionSeconds = 0;
            if (currentTimestamp > joinTimestamp) {
                sessionSeconds = currentTimestamp - joinTimestamp;
            }

            auto oldVal = db->get(uuid);
            uint64_t totalSeconds = sessionSeconds;
            if (oldVal) {
                totalSeconds += std::stoull(*oldVal);
            }
            db->set(uuid, std::to_string(totalSeconds));

            getSelf().getLogger().info(
                "[PlayerTimeTracker] Flushing online player UUID: {}, "
                "session: {}s, total: {}s",
                uuid, sessionSeconds, totalSeconds);
        }
        joinTimes.clear();
    }

    ll::mod::NativeMod& mSelf;
};

} // namespace playertime

LL_REGISTER_MOD(playertime::PlayerTimeTracker, playertime::PlayerTimeTracker::getInstance());