#include <SFML/Graphics.hpp>
#include <windows.h>
#include <deque>
#include <chrono>
#include <cstdlib>
#include <cmath>

using Clock = std::chrono::steady_clock;
using TP    = Clock::time_point;
using Ms    = std::chrono::milliseconds;
using Fsec  = std::chrono::duration<float>;

enum class State { Normal, Smile, Laugh, Cry, Drag };

// ---------- system monitor ------------------------------------------------
struct SystemMonitor {
    FILETIME prevIdle{}, prevKernel{}, prevUser{};
    float    cpuUsage    = 0.f;
    float    memUsage    = 0.f;
    TP       lastSample  = {};
    bool     initialized = false;

    static unsigned long long ft2ull(FILETIME ft) {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    void sample() {
        TP now = Clock::now();
        if (initialized &&
            std::chrono::duration_cast<Ms>(now - lastSample).count() < 2000)
            return;

        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return;

        if (initialized) {
            auto dIdle   = ft2ull(idle)   - ft2ull(prevIdle);
            auto dKernel = ft2ull(kernel) - ft2ull(prevKernel);
            auto dUser   = ft2ull(user)   - ft2ull(prevUser);
            auto total   = dKernel + dUser;
            if (total > 0)
                cpuUsage = static_cast<float>(total - dIdle) / total * 100.f;
        }

        prevIdle   = idle;
        prevKernel = kernel;
        prevUser   = user;

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
            memUsage = static_cast<float>(ms.dwMemoryLoad);

        lastSample  = now;
        initialized = true;
    }

    bool isBusy() const { return cpuUsage > 70.f || memUsage > 80.f; }
};

// ---------- squish animation ----------------------------------------------
struct Squish {
    enum class Phase { None, Compress, Restore };
    Phase phase      = Phase::None;
    TP    phaseStart = {};
    bool  vertical   = false; // true = hit top/bottom, false = hit left/right

    bool isActive() const { return phase != Phase::None; }

    void trigger(bool isVert, TP now) {
        phase      = Phase::Compress;
        phaseStart = now;
        vertical   = isVert;
    }

    // Returns {scaleX, scaleY}; also advances phase internally.
    sf::Vector2f getScale(TP now) {
        if (phase == Phase::None) return {1.f, 1.f};

        float elapsed = std::chrono::duration_cast<Fsec>(now - phaseStart).count();
        float sq, ex;

        if (phase == Phase::Compress) {
            float t = std::min(elapsed / 0.10f, 1.f);
            sq = 1.f - 0.4f * t; // 1.0 -> 0.6
            ex = 1.f + 0.4f * t; // 1.0 -> 1.4
            if (t >= 1.f) { phase = Phase::Restore; phaseStart = now; }
        } else {
            float t = std::min(elapsed / 0.15f, 1.f);
            sq = 0.6f + 0.4f * t; // 0.6 -> 1.0
            ex = 1.4f - 0.4f * t; // 1.4 -> 1.0
            if (t >= 1.f) { phase = Phase::None; return {1.f, 1.f}; }
        }

        // vertical hit: flatten Y, widen X
        return vertical ? sf::Vector2f{ex, sq} : sf::Vector2f{sq, ex};
    }
};

// ---------- physics -------------------------------------------------------
struct Physics {
    sf::Vector2f vel    = {};
    bool         active = false;
    Squish       squish;

    static constexpr float GRAVITY    = 0.4f;
    static constexpr float AIR_DRAG   = 0.98f;
    static constexpr float BOUNCE_K   = 0.70f;
    static constexpr float STOP_SPEED = 0.5f;
    static constexpr int   WIN_SIZE   = 300;

    void launch(sf::Vector2f v) { vel = v; active = true; squish = {}; }
    void stop()                  { active = false; vel = {}; squish = {}; }

    void update(sf::RenderWindow& window, TP now) {
        vel.y += GRAVITY;
        vel   *= AIR_DRAG;

        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        sf::Vector2i pos = window.getPosition();
        pos.x += static_cast<int>(vel.x);
        pos.y += static_cast<int>(vel.y);

        if (pos.x < 0) {
            pos.x = 0;
            vel.x = std::abs(vel.x) * BOUNCE_K;
            if (vel.x < 2.f) vel.x = 0.f;
            squish.trigger(false, now);
        } else if (pos.x > sw - WIN_SIZE) {
            pos.x = sw - WIN_SIZE;
            vel.x = -std::abs(vel.x) * BOUNCE_K;
            if (std::abs(vel.x) < 2.f) vel.x = 0.f;
            squish.trigger(false, now);
        }
        if (pos.y < 0) {
            pos.y = 0;
            vel.y = std::abs(vel.y) * BOUNCE_K;
            if (vel.y < 2.f) vel.y = 0.f;
            squish.trigger(true, now);
        } else if (pos.y > sh - WIN_SIZE) {
            pos.y = sh - WIN_SIZE;
            vel.y = -std::abs(vel.y) * BOUNCE_K;
            if (std::abs(vel.y) < 2.f) vel.y = 0.f;
            squish.trigger(true, now);
        }

        window.setPosition(pos);

        if (std::hypot(vel.x, vel.y) < STOP_SPEED && !squish.isActive())
            active = false;
    }
};

// ---------- main ----------------------------------------------------------
int main()
{
    sf::RenderWindow window(
        sf::VideoMode({300, 300}),
        "nailong-pet",
        sf::Style::None,
        sf::State::Windowed
    );
    window.setFramerateLimit(60);

    HWND hwnd = window.getNativeHandle();
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    State state    = State::Normal;
    TP    stateEnd = {};

    SystemMonitor sysmon;
    Physics       phys;

    std::deque<TP> clicks;
    constexpr int  LAUGH_N  = 5;
    constexpr int  RAPID_MS = 1500;

    bool         dragging = false;
    bool         dragged  = false;
    sf::Vector2i pressPos {};
    sf::Vector2i dragOff  {};

    // position history for throw velocity (screen coords + timestamp)
    struct PosStamp { sf::Vector2i pos; TP t; };
    std::deque<PosStamp> posHistory;

    while (window.isOpen()) {
        TP now = Clock::now();
        sysmon.sample();

        while (auto ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) {
                window.close();

            } else if (auto* e = ev->getIf<sf::Event::KeyPressed>()) {
                if (e->code == sf::Keyboard::Key::Escape)
                    window.close();

            } else if (auto* e = ev->getIf<sf::Event::MouseButtonPressed>()) {
                if (e->button == sf::Mouse::Button::Left) {
                    phys.stop();
                    dragging = true;
                    dragged  = false;
                    posHistory.clear();
                    pressPos = sf::Mouse::getPosition();
                    dragOff  = pressPos - window.getPosition();
                } else if (e->button == sf::Mouse::Button::Right) {
                    window.close();
                }

            } else if (ev->is<sf::Event::MouseMoved>()) {
                if (dragging) {
                    sf::Vector2i cur = sf::Mouse::getPosition();
                    auto d = cur - pressPos;
                    if (std::abs(d.x) > 5 || std::abs(d.y) > 5)
                        dragged = true;
                    window.setPosition(cur - dragOff);

                    // record for throw velocity; keep only last 100ms
                    posHistory.push_back({cur, now});
                    while (posHistory.size() > 1) {
                        float age = std::chrono::duration_cast<Fsec>(
                                        now - posHistory.front().t).count();
                        if (age > 0.10f) posHistory.pop_front();
                        else             break;
                    }
                }

            } else if (auto* e = ev->getIf<sf::Event::MouseButtonReleased>()) {
                if (e->button == sf::Mouse::Button::Left) {
                    bool wasClick = dragging && !dragged;
                    dragging = false;

                    // compute throw velocity from position history
                    if (dragged && posHistory.size() >= 2) {
                        auto& first = posHistory.front();
                        auto& last  = posHistory.back();
                        float dt = std::chrono::duration_cast<Fsec>(
                                       last.t - first.t).count();
                        float totalDisp = std::hypot(
                            (float)(last.pos.x - pressPos.x),
                            (float)(last.pos.y - pressPos.y));
                        if (dt > 0.001f) {
                            sf::Vector2f v = {
                                (float)(last.pos.x - first.pos.x) / dt / 60.f,
                                (float)(last.pos.y - first.pos.y) / dt / 60.f
                            };
                            if (std::hypot(v.x, v.y) > 15.f && totalDisp > 50.f)
                                phys.launch(v);
                        }
                    }
                    posHistory.clear();

                    if (wasClick && state != State::Laugh) {
                        clicks.push_back(now);
                        while (!clicks.empty()) {
                            auto age = std::chrono::duration_cast<Ms>(
                                           now - clicks.front()).count();
                            if (age > RAPID_MS) clicks.pop_front();
                            else                break;
                        }
                        if (static_cast<int>(clicks.size()) >= LAUGH_N) {
                            state    = State::Laugh;
                            stateEnd = now + Ms(2000);
                            clicks.clear();
                        } else {
                            state    = State::Smile;
                            stateEnd = now + Ms(1000);
                        }
                    }
                }
            }
        }

        // --- state priority: Physics > Drag > Click > Cry > Normal ---
        if (phys.active) {
            phys.update(window, now);
            // state variable intentionally left alone during flight;
            // render branch below uses phys.active directly.
        } else if (dragging && dragged) {
            state = State::Drag;
        } else if (state == State::Drag) {
            state = sysmon.isBusy() ? State::Cry : State::Normal;
        } else if (state == State::Smile || state == State::Laugh) {
            if (now >= stateEnd)
                state = sysmon.isBusy() ? State::Cry : State::Normal;
        } else {
            state = sysmon.isBusy() ? State::Cry : State::Normal;
        }

        // --- render ---
        window.clear(sf::Color::Black);

        if (phys.active) {
            // flying / squishing: normal circle with squish scale applied
            constexpr float r = 80.f;
            sf::Vector2f scale = phys.squish.getScale(now);
            sf::CircleShape circle(r);
            circle.setFillColor(sf::Color(255, 140, 0));
            circle.setScale(scale);
            circle.setPosition({150.f - r * scale.x, 150.f - r * scale.y});
            window.draw(circle);

        } else if (state == State::Drag) {
            constexpr float rx = 105.f, ry = 58.f;
            sf::CircleShape ellipse(rx);
            ellipse.setScale({1.f, ry / rx});
            ellipse.setFillColor(sf::Color(150, 50, 220));
            ellipse.setPosition({150.f - rx, 150.f - ry});
            window.draw(ellipse);

        } else {
            float     r = 80.f;
            sf::Color c = sf::Color(255, 140, 0);
            switch (state) {
            case State::Normal: r =  80.f; c = sf::Color(255, 140,   0); break;
            case State::Smile:  r =  90.f; c = sf::Color(255, 220,   0); break;
            case State::Laugh:  r = 110.f; c = sf::Color(220,  50,  50); break;
            case State::Cry:    r =  60.f; c = sf::Color( 50, 100, 255); break;
            default:                                                       break;
            }
            sf::CircleShape circle(r);
            circle.setFillColor(c);
            circle.setPosition({150.f - r, 150.f - r});
            window.draw(circle);
        }

        window.display();
    }

    return 0;
}
