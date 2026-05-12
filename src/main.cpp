// ============================================================
// nailong-pet —— 桌面宠物主程序
// 功能概览：
//   1. 透明无边框窗口，始终置顶
//   2. 鼠标点击触发"微笑/大笑"状态
//   3. CPU/内存过载时触发"哭泣"状态
//   4. 拖动窗口时显示"拖动"状态（紫色椭圆）
//   5. 快速甩动后触发物理飞行 + 边缘弹跳挤压动画
// ============================================================

#include <SFML/Graphics.hpp>
#include <windows.h>
#include <deque>
#include <chrono>
#include <cstdlib>
#include <cmath>

// 时间工具别名，方便后续使用
using Clock = std::chrono::steady_clock;   // 单调时钟，不受系统时间调整影响
using TP    = Clock::time_point;           // 时间点类型
using Ms    = std::chrono::milliseconds;   // 毫秒单位
using Fsec  = std::chrono::duration<float>; // 浮点秒，用于物理计算

// ============================================================
// 桌宠状态枚举
//   Normal  —— 默认橙色圆形
//   Smile   —— 点击后变黄、微微变大
//   Laugh   —— 快速点击5次后变红、明显变大
//   Cry     —— CPU或内存过载时变蓝、缩小
//   Drag    —— 鼠标拖动时变紫色椭圆
// ============================================================
enum class State { Normal, Smile, Laugh, Cry, Drag };


// ============================================================
// SystemMonitor —— 系统资源监控器
// 每隔2秒采样一次 CPU 和内存使用率，避免频繁调用 API 影响性能。
// CPU  使用率：通过 GetSystemTimes 计算空闲时间占比
// 内存使用率：通过 GlobalMemoryStatusEx 直接读取百分比
// ============================================================
struct SystemMonitor {
    // 上一次采样时的系统时间快照（用于计算两次采样间的差值）
    FILETIME prevIdle{}, prevKernel{}, prevUser{};
    float    cpuUsage    = 0.f;   // 当前 CPU 使用率（0~100）
    float    memUsage    = 0.f;   // 当前内存使用率（0~100）
    TP       lastSample  = {};    // 上次采样的时间点
    bool     initialized = false; // 是否已完成第一次采样（需要两次才能计算差值）

    // 将 Windows FILETIME 结构（高32位+低32位）合并成 64 位整数
    // FILETIME 的单位是 100 纳秒，合并后便于做减法求差值
    static unsigned long long ft2ull(FILETIME ft) {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    // 每帧调用，但内部节流：距上次采样不足2秒则直接返回
    void sample() {
        TP now = Clock::now();
        // 节流：2秒内不重复采样
        if (initialized &&
            std::chrono::duration_cast<Ms>(now - lastSample).count() < 2000)
            return;

        // 读取当前系统时间（空闲时间、内核时间、用户时间）
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return;

        // 第一次采样只记录基准值，第二次才能计算差值
        if (initialized) {
            // 计算两次采样之间各时间段的增量
            auto dIdle   = ft2ull(idle)   - ft2ull(prevIdle);
            auto dKernel = ft2ull(kernel) - ft2ull(prevKernel);
            auto dUser   = ft2ull(user)   - ft2ull(prevUser);
            // total = 内核时间增量 + 用户时间增量（内核时间已包含空闲时间）
            auto total   = dKernel + dUser;
            // CPU使用率 = (总时间 - 空闲时间) / 总时间 × 100
            if (total > 0)
                cpuUsage = static_cast<float>(total - dIdle) / total * 100.f;
        }

        // 更新基准快照
        prevIdle   = idle;
        prevKernel = kernel;
        prevUser   = user;

        // 读取内存使用率：dwMemoryLoad 直接给出 0~100 的百分比，无需额外计算
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms); // 必须先设置结构体大小，否则 API 返回失败
        if (GlobalMemoryStatusEx(&ms))
            memUsage = static_cast<float>(ms.dwMemoryLoad);

        lastSample  = now;
        initialized = true;
    }

    // 判断系统是否繁忙：CPU 超过70% 或内存超过80% 即触发哭泣状态
    bool isBusy() const { return cpuUsage > 70.f || memUsage > 80.f; }
};


// ============================================================
// Squish —— 碰撞挤压动画
// 奶龙撞到屏幕边缘时播放两阶段形变动画：
//   压缩阶段（100ms）：沿碰撞方向压扁，垂直方向拉伸
//   复原阶段（150ms）：缓慢弹回原始比例
// 通过对 CircleShape 施加 setScale 实现椭圆化效果。
// ============================================================
struct Squish {
    // 动画所处阶段
    enum class Phase { None, Compress, Restore };
    Phase phase      = Phase::None;
    TP    phaseStart = {};     // 当前阶段开始的时间点
    bool  vertical   = false;  // true = 撞上下边缘，false = 撞左右边缘

    // 动画是否正在播放
    bool isActive() const { return phase != Phase::None; }

    // 触发一次挤压动画（由物理系统在检测到碰撞时调用）
    // isVert：true 表示撞击上/下边缘，false 表示撞击左/右边缘
    void trigger(bool isVert, TP now) {
        phase      = Phase::Compress; // 从压缩阶段开始
        phaseStart = now;
        vertical   = isVert;
    }

    // 每帧调用，返回当前应施加给圆形的缩放系数 {scaleX, scaleY}
    // 同时负责在阶段结束时自动推进到下一阶段
    sf::Vector2f getScale(TP now) {
        if (phase == Phase::None) return {1.f, 1.f}; // 无动画，正常比例

        // 计算当前阶段已经过去的时间（秒）
        float elapsed = std::chrono::duration_cast<Fsec>(now - phaseStart).count();
        float sq, ex; // sq = 压缩轴比例，ex = 拉伸轴比例

        if (phase == Phase::Compress) {
            // 压缩阶段持续 0.10 秒，t 从 0 线性增长到 1
            float t = std::min(elapsed / 0.10f, 1.f);
            sq = 1.f - 0.4f * t; // 压缩轴：1.0 → 0.6
            ex = 1.f + 0.4f * t; // 拉伸轴：1.0 → 1.4
            // 压缩阶段结束，切换到复原阶段
            if (t >= 1.f) { phase = Phase::Restore; phaseStart = now; }
        } else {
            // 复原阶段持续 0.15 秒，t 从 0 线性增长到 1
            float t = std::min(elapsed / 0.15f, 1.f);
            sq = 0.6f + 0.4f * t; // 压缩轴：0.6 → 1.0
            ex = 1.4f - 0.4f * t; // 拉伸轴：1.4 → 1.0
            // 复原完毕，结束动画
            if (t >= 1.f) { phase = Phase::None; return {1.f, 1.f}; }
        }

        // 根据碰撞方向决定哪个轴被压缩、哪个轴被拉伸
        // 撞上/下边缘：Y 轴压缩（sq），X 轴拉伸（ex）→ 圆形变扁
        // 撞左/右边缘：X 轴压缩（sq），Y 轴拉伸（ex）→ 圆形变窄
        return vertical ? sf::Vector2f{ex, sq} : sf::Vector2f{sq, ex};
    }
};


// ============================================================
// Physics —— 物理飞行系统
// 奶龙被快速甩出后进入飞行状态，每帧模拟：
//   重力加速度：vel.y 每帧增加 GRAVITY（向下加速）
//   空气阻力：速度每帧乘以 AIR_DRAG（逐渐减速）
//   边缘反弹：撞到屏幕边缘后速度反向并损耗能量，同时触发挤压动画
//   停止条件：速度归零且挤压动画结束后，退出飞行状态
// ============================================================
struct Physics {
    sf::Vector2f vel    = {};     // 当前速度向量（像素/帧）
    bool         active = false;  // 是否处于飞行状态
    Squish       squish;          // 关联的挤压动画实例

    // 物理常数（全部以"像素/帧"为单位，帧率60fps）
    static constexpr float GRAVITY    = 0.4f;   // 每帧重力加速度（像素/帧²）
    static constexpr float AIR_DRAG   = 0.98f;  // 空气阻力衰减系数（每帧速度×0.98）
    static constexpr float BOUNCE_K   = 0.70f;  // 反弹能量保留比例（损耗30%）
    static constexpr float STOP_SPEED = 0.5f;   // 速度低于此值时停止运动（像素/帧）
    static constexpr int   WIN_SIZE   = 300;    // 窗口尺寸（宽高均为300像素）

    // 以给定初速度启动飞行（速度单位：像素/帧）
    void launch(sf::Vector2f v) { vel = v; active = true; squish = {}; }

    // 强制停止飞行（例如用户点击抓住窗口时调用）
    void stop() { active = false; vel = {}; squish = {}; }

    // 每帧更新物理状态：移动窗口、处理边缘碰撞、检查停止条件
    void update(sf::RenderWindow& window, TP now) {
        // 施加重力（Y轴正方向为屏幕向下）
        vel.y += GRAVITY;
        // 施加空气阻力（所有方向速度等比衰减）
        vel   *= AIR_DRAG;

        // 获取屏幕分辨率（每帧读取以支持多显示器切换）
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        // 根据速度向量计算新位置
        sf::Vector2i pos = window.getPosition();
        pos.x += static_cast<int>(vel.x);
        pos.y += static_cast<int>(vel.y);

        // ---- 边缘碰撞检测：先 clamp 位置，再处理速度反弹 ----
        // 必须先将位置限制在边界内，否则下一帧仍会检测到超界，导致反复反弹

        // 检测左边缘碰撞
        if (pos.x < 0) {
            pos.x = 0; // 强制对齐到左边缘
            // 速度过小时直接归零（避免低速下无限振动），否则反弹并损耗能量
            if (std::abs(vel.x) < 3.f) vel.x = 0.f;
            else                        vel.x = std::abs(vel.x) * BOUNCE_K; // 反向取正值
            squish.trigger(false, now); // 触发左右方向的挤压动画
        } else if (pos.x > sw - WIN_SIZE) {
            // 检测右边缘碰撞
            pos.x = sw - WIN_SIZE; // 强制对齐到右边缘
            if (std::abs(vel.x) < 3.f) vel.x = 0.f;
            else                        vel.x = -std::abs(vel.x) * BOUNCE_K; // 反向取负值
            squish.trigger(false, now);
        }

        // 检测上边缘碰撞
        if (pos.y < 0) {
            pos.y = 0;
            if (std::abs(vel.y) < 3.f) vel.y = 0.f;
            else                        vel.y = std::abs(vel.y) * BOUNCE_K;
            squish.trigger(true, now); // 触发上下方向的挤压动画
        } else if (pos.y > sh - WIN_SIZE) {
            // 检测下边缘碰撞（屏幕底部）
            pos.y = sh - WIN_SIZE;
            if (std::abs(vel.y) < 3.f) vel.y = 0.f;
            else                        vel.y = -std::abs(vel.y) * BOUNCE_K;
            squish.trigger(true, now);
        }

        // 全局速度兜底：合速度低于 1 像素/帧时彻底归零，防止长尾漂移
        if (std::hypot(vel.x, vel.y) < 1.f) { vel.x = 0.f; vel.y = 0.f; }

        // 应用新位置
        window.setPosition(pos);

        // 停止条件：速度完全为零 且 挤压动画已结束
        if (vel.x == 0.f && vel.y == 0.f && !squish.isActive())
            active = false;
    }
};


// ============================================================
// main —— 程序入口
// 负责：窗口初始化、事件循环、状态机、渲染
// ============================================================
int main()
{
    // ---- 创建无边框、透明背景的 SFML 窗口 ----
    sf::RenderWindow window(
        sf::VideoMode({300, 300}),  // 窗口尺寸 300×300 像素
        "nailong-pet",
        sf::Style::None,            // 无标题栏、无边框
        sf::State::Windowed
    );
    window.setFramerateLimit(60);   // 限制帧率为60fps，控制物理精度和CPU占用

    // 获取底层 Win32 窗口句柄，用于调用 Windows API
    HWND hwnd = window.getNativeHandle();

    // 将窗口置于所有窗口的最上层（包括任务栏），不改变大小和位置
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // 为窗口添加 WS_EX_LAYERED 扩展样式，这是使用颜色键透明的前提
    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);

    // 设置颜色键：纯黑色（0,0,0）的像素变为完全透明
    // 这样 SFML 用黑色清屏后，只有彩色的圆形部分可见
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    // ---- 状态机变量 ----
    State state    = State::Normal; // 当前桌宠状态，初始为正常状态
    TP    stateEnd = {};            // 点击状态（Smile/Laugh）的到期时间点

    // 系统监控器和物理系统实例
    SystemMonitor sysmon;
    Physics       phys;

    // ---- 点击计数相关变量 ----
    std::deque<TP> clicks;          // 记录最近点击的时间戳，用于检测快速连击
    constexpr int  LAUGH_N  = 5;    // 触发大笑所需的连击次数
    constexpr int  RAPID_MS = 1500; // 连击有效时间窗口（1.5秒内的点击才算连击）

    // ---- 鼠标拖动相关变量 ----
    bool         dragging = false;  // 鼠标左键是否正在按住
    bool         dragged  = false;  // 本次按住期间是否发生了有效位移（区分点击与拖动）
    sf::Vector2i pressPos {};       // 鼠标按下时的屏幕坐标（用于计算位移和判断拖动）
    sf::Vector2i dragOff  {};       // 鼠标按下时，鼠标相对于窗口左上角的偏移量

    // ---- 甩动速度历史记录 ----
    // 记录拖动过程中的位置和时间，松开时用最近100ms内的轨迹计算初速度
    struct PosStamp { sf::Vector2i pos; TP t; };
    std::deque<PosStamp> posHistory;

    // ============================================================
    // 主循环：事件处理 → 状态更新 → 渲染，每帧执行一次
    // ============================================================
    while (window.isOpen()) {
        TP now = Clock::now(); // 记录本帧开始时间，供本帧所有逻辑统一使用
        sysmon.sample();       // 尝试采样系统资源（内部节流，2秒最多一次）

        // ---- 事件处理 ----
        while (auto ev = window.pollEvent()) {

            // 窗口关闭事件（如任务栏右键关闭）
            if (ev->is<sf::Event::Closed>()) {
                window.close();

            // 键盘事件：按 Escape 退出程序
            } else if (auto* e = ev->getIf<sf::Event::KeyPressed>()) {
                if (e->code == sf::Keyboard::Key::Escape)
                    window.close();

            // 鼠标按下事件
            } else if (auto* e = ev->getIf<sf::Event::MouseButtonPressed>()) {
                if (e->button == sf::Mouse::Button::Left) {
                    // 左键按下：停止飞行状态（允许用户抓住正在飞的奶龙），开始拖动/点击检测
                    phys.stop();
                    dragging = true;
                    dragged  = false;           // 重置位移标志，等待 MouseMoved 判断
                    posHistory.clear();         // 清空速度历史，准备记录新的轨迹
                    pressPos = sf::Mouse::getPosition();          // 记录按下时的屏幕坐标
                    dragOff  = pressPos - window.getPosition();   // 计算鼠标与窗口原点的偏移
                } else if (e->button == sf::Mouse::Button::Right) {
                    // 右键按下：直接退出程序
                    window.close();
                }

            // 鼠标移动事件
            } else if (ev->is<sf::Event::MouseMoved>()) {
                if (dragging) {
                    sf::Vector2i cur = sf::Mouse::getPosition(); // 当前鼠标屏幕坐标
                    auto d = cur - pressPos;
                    // 位移超过5像素才判定为拖动（避免手抖被误判）
                    if (std::abs(d.x) > 5 || std::abs(d.y) > 5)
                        dragged = true;
                    // 将窗口移动到"当前鼠标位置 - 初始偏移"，实现平滑跟手
                    window.setPosition(cur - dragOff);

                    // 记录当前位置和时间到速度历史，同时淘汰100ms以前的旧数据
                    // 只保留最近100ms，确保计算的是"松手前瞬间"的速度
                    posHistory.push_back({cur, now});
                    while (posHistory.size() > 1) {
                        float age = std::chrono::duration_cast<Fsec>(
                                        now - posHistory.front().t).count();
                        if (age > 0.10f) posHistory.pop_front(); // 淘汰100ms前的记录
                        else             break;
                    }
                }

            // 鼠标松开事件
            } else if (auto* e = ev->getIf<sf::Event::MouseButtonReleased>()) {
                if (e->button == sf::Mouse::Button::Left) {
                    // wasClick：按下后没有发生有效位移，判定为点击而非拖动
                    bool wasClick = dragging && !dragged;
                    dragging = false;

                    // ---- 甩动判定 ----
                    // 条件：本次是拖动（dragged=true），且速度历史有足够数据
                    if (dragged && posHistory.size() >= 2) {
                        auto& first = posHistory.front();
                        auto& last  = posHistory.back();
                        // 计算速度历史的时间跨度（秒）
                        float dt = std::chrono::duration_cast<Fsec>(
                                       last.t - first.t).count();
                        // 计算从按下点到松开点的总位移（像素）
                        float totalDisp = std::hypot(
                            (float)(last.pos.x - pressPos.x),
                            (float)(last.pos.y - pressPos.y));
                        if (dt > 0.001f) {
                            // 用时间差计算速度（像素/秒），避免帧率波动影响
                            sf::Vector2f vPps = {
                                (float)(last.pos.x - first.pos.x) / dt,
                                (float)(last.pos.y - first.pos.y) / dt
                            };
                            // 同时满足"速度 > 400像素/秒"且"总位移 > 80像素"才触发甩动
                            // 防止原地抖动或缓慢拖动被误判为甩动
                            if (std::hypot(vPps.x, vPps.y) > 400.f && totalDisp > 80.f)
                                phys.launch({vPps.x / 60.f, vPps.y / 60.f}); // 转换为像素/帧后启动飞行
                        }
                    }
                    posHistory.clear(); // 松开后清空历史，节省内存

                    // ---- 点击计数逻辑（仅在判定为点击时执行）----
                    // 处于大笑状态时忽略点击，不打断大笑动画
                    if (wasClick && state != State::Laugh) {
                        clicks.push_back(now); // 记录本次点击时间

                        // 淘汰1.5秒以前的旧点击记录（超出连击时间窗口）
                        while (!clicks.empty()) {
                            auto age = std::chrono::duration_cast<Ms>(
                                           now - clicks.front()).count();
                            if (age > RAPID_MS) clicks.pop_front();
                            else                break;
                        }

                        // 连击5次 → 触发大笑状态，持续2秒
                        if (static_cast<int>(clicks.size()) >= LAUGH_N) {
                            state    = State::Laugh;
                            stateEnd = now + Ms(2000);
                            clicks.clear(); // 清空计数，下一轮重新累计
                        } else {
                            // 普通点击 → 触发微笑状态，持续1秒
                            state    = State::Smile;
                            stateEnd = now + Ms(1000);
                        }
                    }
                }
            }
        }

        // ============================================================
        // 状态优先级判定（从高到低）：
        //   飞行/弹跳 > 拖动 > 点击(微笑/大笑) > 哭泣 > 正常
        // ============================================================

        if (phys.active) {
            // 最高优先级：飞行状态 —— 每帧推进物理模拟
            phys.update(window, now);
            // 物理本帧刚停止：立即在同帧内完成状态转换，避免渲染阶段读到过时的旧状态而闪烁
            if (!phys.active)
                state = sysmon.isBusy() ? State::Cry : State::Normal;

        } else if (dragging && dragged) {
            // 拖动状态：鼠标按住且发生了有效位移
            state = State::Drag;

        } else if (state == State::Drag) {
            // 拖动刚结束（松开鼠标且本次是拖动）：根据当前系统负载决定回到哪个状态
            state = sysmon.isBusy() ? State::Cry : State::Normal;

        } else if (state == State::Smile || state == State::Laugh) {
            // 点击状态计时到期：根据当前 CPU/内存情况决定回到哭泣还是正常
            if (now >= stateEnd)
                state = sysmon.isBusy() ? State::Cry : State::Normal;

        } else {
            // 默认情况：实时跟随系统负载（每2秒更新一次，由 sysmon.isBusy() 决定）
            state = sysmon.isBusy() ? State::Cry : State::Normal;
        }

        // ============================================================
        // 渲染：根据当前状态绘制对应形状和颜色
        // 黑色背景会被 LWA_COLORKEY 设为透明，只有彩色部分可见
        // ============================================================
        window.clear(sf::Color::Black);

        if (phys.active) {
            // 飞行/弹跳状态：橙色圆形 + 挤压动画缩放
            constexpr float r = 80.f;
            sf::Vector2f scale = phys.squish.getScale(now); // 获取当前帧的挤压缩放系数
            sf::CircleShape circle(r);
            circle.setFillColor(sf::Color(255, 140, 0));    // 橙色，与正常状态一致
            circle.setScale(scale);
            // setScale 从左上角缩放，需要根据实际缩放量重新计算居中位置
            circle.setPosition({150.f - r * scale.x, 150.f - r * scale.y});
            window.draw(circle);

        } else if (state == State::Drag) {
            // 拖动状态：紫色横向椭圆（通过对圆形施加不等比缩放实现）
            constexpr float rx = 105.f, ry = 58.f; // 横向半径大于纵向，形成扁圆
            sf::CircleShape ellipse(rx);
            ellipse.setScale({1.f, ry / rx});               // Y 轴压缩为 rx 的 ry/rx 倍
            ellipse.setFillColor(sf::Color(150, 50, 220));  // 紫色
            ellipse.setPosition({150.f - rx, 150.f - ry}); // 居中
            window.draw(ellipse);

        } else {
            // 普通状态：根据 state 枚举决定颜色和大小
            float     r = 80.f;
            sf::Color c = sf::Color(255, 140, 0);
            switch (state) {
            case State::Normal: r =  80.f; c = sf::Color(255, 140,   0); break; // 橙色，基准大小
            case State::Smile:  r =  90.f; c = sf::Color(255, 220,   0); break; // 黄色，略微变大
            case State::Laugh:  r = 110.f; c = sf::Color(220,  50,  50); break; // 红色，明显变大
            case State::Cry:    r =  60.f; c = sf::Color( 50, 100, 255); break; // 蓝色，缩小
            default:                                                       break;
            }
            sf::CircleShape circle(r);
            circle.setFillColor(c);
            circle.setPosition({150.f - r, 150.f - r}); // 始终居中于 300×300 窗口
            window.draw(circle);
        }

        window.display(); // 将缓冲区内容提交到屏幕
    }

    return 0;
}
