#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <chrono>

using namespace geode::prelude;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Slot {
    bool physDown = false;      // палец реально на экране
    bool virtDown = false;      // игра считает кнопку зажатой
    bool pendingPress = false;
    bool pendingRelease = false;
    double pressAt = 0;
    double releaseAt = 0;
    double lastReal = -1;       // время последнего настоящего тапа
    double lastEmit = -1000;    // время последнего нажатия, отправленного в игру
    double interval = 0;        // сглаженный интервал спама
    bool spam = false;
};

struct Cfg {
    bool enabled, autofill;
    double boost, maxDelay, hold, minGap;
};

static Cfg readCfg() {
    auto m = Mod::get();
    Cfg c;
    c.enabled  = m->getSettingValue<bool>("enabled");
    c.autofill = m->getSettingValue<bool>("autofill");
    c.boost    = m->getSettingValue<int64_t>("boost") / 100.0;
    c.maxDelay = m->getSettingValue<int64_t>("max-delay") / 1000.0;
    c.hold     = m->getSettingValue<int64_t>("hold") / 100.0;
    c.minGap   = 1.0 / std::max<int64_t>(1, m->getSettingValue<int64_t>("max-cps"));
    return c;
}

class $modify(SpamLayer, GJBaseGameLayer) {
    struct Fields {
        bool injecting = false;
        Slot slots[2];
    };

    bool inPlay() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this);
    }

    // отправить нажатие/отпускание в игру в обход нашей логики
    void emit(int idx, bool down) {
        auto& s = m_fields->slots[idx];
        if (down == s.virtDown) return;
        s.virtDown = down;
        if (down) s.lastEmit = nowSec();
        m_fields->injecting = true;
        GJBaseGameLayer::handleButton(down, 1, idx == 0);
        m_fields->injecting = false;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        Cfg c = readCfg();
        if (button != 1 || m_fields->injecting || !c.enabled || !inPlay()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int idx = isPlayer1 ? 0 : 1;
        auto& s = m_fields->slots[idx];
        double now = nowSec();

        if (down) {
            s.physDown = true;

            // если что-то ещё висит в очереди, сначала отправляем это
            if (s.pendingPress) { s.pendingPress = false; emit(idx, true); }
            if (s.pendingRelease) { s.pendingRelease = false; emit(idx, false); }

            double gap = s.lastReal >= 0 ? now - s.lastReal : 999.0;
            if (gap < 0.35) {
                s.interval = s.interval > 0 ? s.interval + (gap - s.interval) * 0.35 : gap;
                s.spam = true;
            } else {
                s.interval = 0;
                s.spam = false;
            }
            s.lastReal = now;

            if (!s.spam) { emit(idx, true); return; }

            double grid = std::max(s.interval * (1.0 - c.boost), c.minGap);
            double slot = s.lastEmit + grid;
            double wait = slot - now;
            if (wait <= 0.0 || wait > c.maxDelay) {
                emit(idx, true);           // опоздал или слишком рано: кликаем сразу
            } else {
                s.pendingPress = true;     // чуть подождать, чтобы попасть в ритм
                s.pressAt = slot;
            }
            return;
        }

        // отпускание
        s.physDown = false;
        if (!s.spam) { emit(idx, false); return; }

        double hold = s.interval * c.hold;
        if (s.pendingPress) {
            s.pendingRelease = true;
            s.releaseAt = s.pressAt + hold;
            return;
        }
        double at = s.lastEmit + hold;
        if (at <= now) emit(idx, false);
        else { s.pendingRelease = true; s.releaseAt = at; }
    }

    void tick() {
        Cfg c = readCfg();
        double t = nowSec();
        for (int i = 0; i < 2; i++) {
            auto& s = m_fields->slots[i];
            if (s.pendingPress && t >= s.pressAt) { s.pendingPress = false; emit(i, true); }
            if (s.pendingRelease && !s.pendingPress && t >= s.releaseAt) { s.pendingRelease = false; emit(i, false); }

            if (c.autofill && s.spam && s.physDown && s.interval > 0 && !s.pendingPress) {
                double grid = std::max(s.interval * (1.0 - c.boost), c.minGap);
                double hold = grid * c.hold;
                if (s.virtDown && t >= s.lastEmit + hold) emit(i, false);
                else if (!s.virtDown && t >= s.lastEmit + grid) emit(i, true);
            }
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isHalfTick && inPlay() && Mod::get()->getSettingValue<bool>("enabled")) {
            tick();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
