// ============================================================
// nailong-pet —— 桌面宠物主程序
// 功能概览：
//   1. 透明无边框窗口，始终置顶
//   2. 鼠标点击触发"微笑/大笑"状态
//   3. CPU/内存过载时触发"哭泣"状态
//   4. 拖动窗口时显示"拖动"状态（紫色椭圆）
//   5. 快速甩动后触发物理飞行 + 边缘弹跳挤压动画
//   6. 连续使用60分钟后触发"疲惫"状态（灰色 + "Take a break!"气泡）
//   7. 音效系统：点击/大笑时播放程序化正弦波音效，支持静音标志
//   8. 右键菜单：关闭 / 最小化到托盘 / 固定 / 禁用点击动画 / 静音
//   9. 打招呼气泡：启动和唤醒时显示问候、日期、陪伴时长、IP属地（20秒）
// ============================================================

#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <windows.h>
#include <shellapi.h>   // Shell_NotifyIcon（系统托盘）
#include <winhttp.h>    // WinHttpOpen 等（IP 属地查询）
#include <deque>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <cstdio>
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
//   Tired   —— 连续使用超过60分钟后变灰，并显示"Take a break!"气泡
//   PreCry  —— 系统刚超过负载阈值时的过渡状态，播放1秒后自动切换到 Cry
//   Cry     —— CPU或内存过载时变蓝、缩小
//   Drag    —— 鼠标拖动时变紫色椭圆
// ============================================================
enum class State { Normal, Smile, Laugh, Tired, PreCry, Cry, Drag };


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
    float sqAmt      = 0.4f;  // 压缩量：压缩轴最大变化量（1.0 - sqAmt = 最小比例）
    float exAmt      = 0.4f;  // 拉伸量：拉伸轴最大变化量（1.0 + exAmt = 最大比例）

    // 动画是否正在播放
    bool isActive() const { return phase != Phase::None; }

    // 触发一次挤压动画（由物理系统在检测到碰撞时调用）
    // isVert=true：撞上/下边缘，形变较强（压缩到0.6，拉伸到1.4）
    // isVert=false：撞左/右边缘，形变较弱（压缩到0.8，拉伸到1.2），视觉上不那么剧烈
    void trigger(bool isVert, TP now) {
        phase      = Phase::Compress;
        phaseStart = now;
        vertical   = isVert;
        sqAmt      = isVert ? 0.4f : 0.2f; // 上下：强形变；左右：弱形变
        exAmt      = isVert ? 0.4f : 0.2f;
    }

    // 每帧调用，返回当前应施加给 Sprite 的缩放系数 {scaleX, scaleY}
    // 同时负责在阶段结束时自动推进到下一阶段
    sf::Vector2f getScale(TP now) {
        if (phase == Phase::None) return {1.f, 1.f}; // 无动画，正常比例

        float elapsed = std::chrono::duration_cast<Fsec>(now - phaseStart).count();
        float sq, ex; // sq = 压缩轴当前比例，ex = 拉伸轴当前比例

        if (phase == Phase::Compress) {
            // 压缩阶段持续 0.10 秒，t 从 0 线性增长到 1
            float t = std::min(elapsed / 0.10f, 1.f);
            sq = 1.f - sqAmt * t; // 压缩轴：1.0 → (1.0 - sqAmt)
            ex = 1.f + exAmt * t; // 拉伸轴：1.0 → (1.0 + exAmt)
            if (t >= 1.f) { phase = Phase::Restore; phaseStart = now; }
        } else {
            // 复原阶段持续 0.15 秒，t 从 0 线性增长到 1
            float t = std::min(elapsed / 0.15f, 1.f);
            sq = (1.f - sqAmt) + sqAmt * t; // 压缩轴复原：(1-sqAmt) → 1.0
            ex = (1.f + exAmt) - exAmt * t; // 拉伸轴复原：(1+exAmt) → 1.0
            if (t >= 1.f) { phase = Phase::None; return {1.f, 1.f}; }
        }

        // 根据碰撞方向决定哪个轴被压缩、哪个轴被拉伸
        // 撞上/下边缘：Y 轴压缩（sq），X 轴拉伸（ex）→ 变扁
        // 撞左/右边缘：X 轴压缩（sq），Y 轴拉伸（ex）→ 变窄
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
    int winWidth  = 300; // 窗口宽度（像素），加载素材后更新
    int winHeight = 300; // 窗口高度（像素），加载素材后更新

    // idle_01.png 内容 bounding box（由 compute_body_bbox.py 生成，原始像素单位）
    // 图片实际尺寸 518×718，内容区域相对于图片左上角的偏移及宽高
    static constexpr int IDLE_IMG_W   = 518;
    static constexpr int IDLE_IMG_H   = 718;
    static constexpr int IDLE_BBOX_OX = 94;   // 内容左边距
    static constexpr int IDLE_BBOX_OY = 30;   // 内容上边距
    static constexpr int IDLE_BBOX_W  = 329;  // 内容宽度
    static constexpr int IDLE_BBOX_H  = 657;  // 内容高度

    // 碰撞检测用的身体矩形偏移（相对于窗口边缘，运行时由 setupBodyRect 计算）
    // bodyOffL/T：窗口左/上边到身体左/上边的距离（像素）
    // bodyOffR/B：身体右/下边到窗口右/下边的距离（像素）
    int bodyOffL = 0, bodyOffT = 0, bodyOffR = 0, bodyOffB = 0;

    // 根据 idle 动画的实际 displayScale 计算碰撞矩形偏移，素材加载后调用一次
    void setupBodyRect(float scaleX, float scaleY) {
        // Sprite 以纹理中心为原点居中于窗口，推算纹理左上角在窗口内的坐标
        float texL = winWidth  / 2.f - IDLE_IMG_W / 2.f * scaleX;
        float texT = winHeight / 2.f - IDLE_IMG_H / 2.f * scaleY;
        float bL   = texL + IDLE_BBOX_OX * scaleX;
        float bT   = texT + IDLE_BBOX_OY * scaleY;
        float bR   = bL   + IDLE_BBOX_W  * scaleX;
        float bB   = bT   + IDLE_BBOX_H  * scaleY;
        bodyOffL = static_cast<int>(bL);
        bodyOffT = static_cast<int>(bT);
        bodyOffR = static_cast<int>(winWidth  - bR);
        bodyOffB = static_cast<int>(winHeight - bB);
    }

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

        // ---- 边缘碰撞检测：以奶龙身体矩形（而非窗口边框）为基准 ----
        // bodyOffL/T/R/B 由 setupBodyRect 预算好，使反弹恰好发生在身体触边时。
        // 必须先 clamp 位置再处理速度，否则下一帧仍超界导致反复反弹。

        // 左边：身体左边 = pos.x + bodyOffL，触屏左 → pos.x = -bodyOffL
        if (pos.x < -bodyOffL) {
            pos.x = -bodyOffL;
            if (std::abs(vel.x) < 3.f) vel.x = 0.f;
            else                        vel.x = std::abs(vel.x) * BOUNCE_K;
            squish.trigger(false, now);
        } else if (pos.x > sw - winWidth + bodyOffR) {
            // 右边：身体右边 = pos.x + winWidth - bodyOffR，触屏右 → pos.x = sw - winWidth + bodyOffR
            pos.x = sw - winWidth + bodyOffR;
            if (std::abs(vel.x) < 3.f) vel.x = 0.f;
            else                        vel.x = -std::abs(vel.x) * BOUNCE_K;
            squish.trigger(false, now);
        }

        // 上边：身体上边 = pos.y + bodyOffT，触屏顶 → pos.y = -bodyOffT
        if (pos.y < -bodyOffT) {
            pos.y = -bodyOffT;
            if (std::abs(vel.y) < 3.f) vel.y = 0.f;
            else                        vel.y = std::abs(vel.y) * BOUNCE_K;
            squish.trigger(true, now);
        } else if (pos.y > sh - winHeight + bodyOffB) {
            // 下边：身体下边 = pos.y + winHeight - bodyOffB，触屏底 → pos.y = sh - winHeight + bodyOffB
            pos.y = sh - winHeight + bodyOffB;
            if (std::abs(vel.y) < 3.f) vel.y = 0.f;
            else                        vel.y = -std::abs(vel.y) * BOUNCE_K;
            squish.trigger(true, now);
        }

        // 全局速度兜底：贴近屏幕底部且速度极低时停止，防止顶点悬停误判
        bool nearBottom = (pos.y >= sh - winHeight + bodyOffB - 5);
        if (std::hypot(vel.x, vel.y) < 1.f && nearBottom) {
            vel.x = 0.f;
            vel.y = 0.f;
        }

        // 应用新位置
        window.setPosition(pos);

        // 停止条件：速度完全为零 且 挤压动画已结束
        if (vel.x == 0.f && vel.y == 0.f && !squish.isActive())
            active = false;
    }
};


// laugh 动画渲染宽度基准（像素）。
// laugh 按 TARGET_W / laugh帧宽 等比缩放；idle 和 smile 的 X/Y 缩放分别对齐到
// laugh 的渲染尺寸，确保三套动画在屏幕上呈现完全相同的视觉大小。
constexpr float TARGET_W = 180.f;

// ============================================================
// Animation —— 帧动画播放器
// 从磁盘加载一系列连续编号的 PNG 帧（prefix_01.png …），按固定帧率循环播放。
// 加载失败时 loaded=false，调用方据此回退到占位图形，不会崩溃。
// ============================================================
struct Animation {
    std::vector<sf::Texture> frames;        // 所有帧的纹理（按顺序存储）
    std::size_t              frameIdx = 0;  // 当前显示帧的索引
    float                    fps      = 12.f; // 播放帧率（帧/秒）
    float                    elapsed  = 0.f;  // 当前帧内已累计的时间（秒）
    bool                     loaded   = false;
    bool                     loop     = true;    // true=循环播放，false=播完最后一帧停住
    // X/Y 独立缩放系数，由外部以 laugh 为基准统一校准，
    // 保证 idle/smile/laugh 三套动画渲染后像素尺寸完全一致。
    sf::Vector2f             displayScale = {1.f, 1.f};

    // 从 folder/prefix_01.png … prefix_NN.png 加载 count 帧
    // 任意一帧加载失败则整组丢弃，返回 false
    bool load(const std::string& folder, const std::string& prefix, int count) {
        frames.clear();
        frames.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) {
            // 补零到两位，例如 1 → "01"
            std::string num = std::to_string(i);
            if (num.size() < 2) num = "0" + num;
            std::string path = folder + "/" + prefix + "_" + num + ".png";
            sf::Texture tex;
            if (!tex.loadFromFile(path)) {
                fprintf(stderr, "[Animation] 加载失败: %s\n", path.c_str());
                frames.clear();
                loaded = false;
                return false;
            }
            frames.push_back(std::move(tex));
        }
        loaded   = !frames.empty();
        frameIdx = 0;
        elapsed  = 0.f;
        return loaded;
    }

    // 重置到第一帧（状态切入时调用，使动画从头开始）
    void reset() { frameIdx = 0; elapsed = 0.f; }

    // 每帧调用：累加 deltaTime，按帧率推进帧索引
    // loop=true 时循环，loop=false 时停在最后一帧
    void update(float dt) {
        if (!loaded || frames.empty()) return;
        elapsed += dt;
        float dur = 1.f / fps;
        while (elapsed >= dur) {
            elapsed -= dur;
            if (frameIdx + 1 < frames.size())
                ++frameIdx;
            else if (loop)
                frameIdx = 0;
            // loop=false 且已到末帧：保持停在最后一帧，不重置
        }
    }

    // 返回当前帧的 Sprite，以纹理中心为原点并居中于 winSize 所描述的窗口。
    // 缩放使用 displayScale（由外部以 laugh 为基准校准），squish 为物理挤压附加系数。
    sf::Sprite getCurrentSprite(sf::Vector2u winSize,
                                sf::Vector2f squish = {1.f, 1.f}) const {
        const sf::Texture& tex = frames[frameIdx];
        sf::Sprite spr(tex);
        auto ts = tex.getSize();
        spr.setOrigin({ts.x / 2.f, ts.y / 2.f});
        spr.setScale({squish.x * displayScale.x, squish.y * displayScale.y});
        spr.setPosition({winSize.x / 2.f, winSize.y / 2.f});
        return spr;
    }
};


// ============================================================
// makeSineBuffer —— 程序化生成正弦波音效缓冲区
// 不依赖外部音频文件，通过数学计算直接写入 PCM 样本。
// freq:       音调频率（Hz），例如 660 = E5，880 = A5
// duration:   时长（秒）
// sampleRate: 采样率（默认 44100 Hz）
// 末尾施加线性淡出包络，防止截断噪声（爆音）。
// ============================================================
sf::SoundBuffer makeSineBuffer(float freq, float duration, unsigned int sampleRate = 44100) {
    auto numSamples = static_cast<std::size_t>(sampleRate * duration);
    std::vector<std::int16_t> samples(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i) {
        float t        = static_cast<float>(i) / static_cast<float>(sampleRate);
        float envelope = 1.f - t / duration; // 线性淡出：开头最响，末尾归零
        samples[i]     = static_cast<std::int16_t>(
            envelope * std::sin(2.f * 3.14159265358979f * freq * t) * 28000.f);
    }
    sf::SoundBuffer buf;
    (void)buf.loadFromSamples(samples.data(), samples.size(), 1, sampleRate,
                              {sf::SoundChannel::Mono});
    return buf;
}


// ============================================================
// 托盘回调系统
// Shell_NotifyIcon 把鼠标事件发给 nid.hWnd 指定的窗口。
// 若直接用 SFML 的 hwnd，SFML 内部 PeekMessage(0,0,PM_REMOVE) 会
// 在我们读到消息之前将其吞噬。
// 解决方案：注册一个独立的隐藏消息专用窗口（HWND_MESSAGE），
// 托盘消息只发往该窗口，完全脱离 SFML 消息泵。
// ============================================================

// ============================================================
// 打招呼气泡 —— 程序启动和电脑唤醒时显示
// ============================================================

// ---- IP 属地查询（子线程执行，避免阻塞主线程）----
static std::mutex        s_ipMutex;
static std::string       s_ipLocation = "loading..."; // 初始占位，查询完成后替换
static std::atomic<bool> s_ipReady{false};

// UTF-8 字节串 → wstring（供 sf::String 直接使用）
static std::wstring utf8ToWide(const std::string& u8) {
    if (u8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, &w[0], n);
    return w;
}

// 通过 WinHTTP 向 ip-api.com 查询 IP 属地，结果写入 s_ipLocation（UTF-8）
static void fetchIpLocation() {
    auto setResult = [](const char* loc) {
        std::lock_guard<std::mutex> lk(s_ipMutex);
        s_ipLocation = loc;
        s_ipReady    = true;
    };

    HINTERNET hSess = WinHttpOpen(L"NailongPet/1.0",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { setResult("unknown"); return; }

    HINTERNET hConn = WinHttpConnect(hSess, L"ip-api.com",
                                      INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); setResult("unknown"); return; }

    HINTERNET hReq = WinHttpOpenRequest(
        hConn, L"GET",
        L"/json/?fields=regionName,city",   // 不加 lang=zh-CN，返回英文城市名
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        setResult("unknown"); return;
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);

    std::string body;
    if (ok) {
        char buf[1024]; DWORD read = 0;
        while (WinHttpReadData(hReq, buf, sizeof(buf) - 1, &read) && read > 0) {
            buf[read] = '\0';
            body += buf;
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);

    // 手动解析 JSON 字段（避免引入第三方库）
    auto extract = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\":\"";
        auto pos = body.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        auto end = body.find('"', pos);
        return end == std::string::npos ? std::string{} : body.substr(pos, end - pos);
    };
    std::string region = extract("regionName");
    std::string city   = extract("city");
    if (region.empty() && city.empty())
        setResult("unknown");
    else
        setResult((region + (city.empty() ? "" : ", " + city)).c_str());
}

// 根据当前小时返回英文问候语（6-12 早上，12-18 下午，其余晚上）
static std::wstring getGreeting() {
    time_t t = time(nullptr);
    struct tm tm{}; localtime_s(&tm, &t);
    int h = tm.tm_hour;
    if (h >= 6  && h < 12) return L"Good morning!";
    if (h >= 12 && h < 18) return L"Good afternoon!";
    return                         L"Good evening!";
}

// 返回 "Today: YYYY/MM/DD Weekday"（全英文，避免字体缺字）
static std::wstring getDateStr() {
    time_t t = time(nullptr);
    struct tm tm{}; localtime_s(&tm, &t);
    const wchar_t* days[] = {L"Sun",L"Mon",L"Tue",L"Wed",
                              L"Thu",L"Fri",L"Sat"};
    wchar_t buf[64];
    swprintf_s(buf, L"Today: %04d/%02d/%02d %ls",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               days[tm.tm_wday]);
    return buf;
}

// ---- 打招呼气泡 ----
struct GreetBubble {
    bool                      active        = false;
    TP                        hideAt        = {};
    std::vector<std::wstring> lines;           // 各行文字（wstring 供 sf::String 使用）
    bool                      ipLineUpdated = false; // IP 行是否已替换为最终值

    void show(std::vector<std::wstring> ls, TP now) {
        lines         = std::move(ls);
        hideAt        = now + std::chrono::seconds(20);
        active        = true;
        ipLineUpdated = false;
    }

    // 每帧调用：检查超时，并在 IP 查询完成后更新最后一行
    void update(TP now) {
        if (!active) return;
        if (now >= hideAt) { active = false; return; }
        if (!ipLineUpdated && s_ipReady.load()) {
            std::lock_guard<std::mutex> lk(s_ipMutex);
            if (lines.size() >= 4)
                lines[3] = std::wstring(L"Location: ") + utf8ToWide(s_ipLocation);
            ipLineUpdated = true;
        }
    }
};

// ---- 全局标志 ----
// 托盘点击标志：由 TrayWndProc 写入，主循环读取后立即清零
static bool g_restoreFromTray = false;
// 电脑唤醒标志：由 TrayWndProc 的 WM_POWERBROADCAST 设置
static bool g_powerResumed    = false;

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 1) {
        UINT ev = LOWORD(lp); // 低16位是实际鼠标事件（WM_LBUTTONUP / WM_LBUTTONDBLCLK 等）
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK)
            g_restoreFromTray = true;
    } else if (msg == WM_POWERBROADCAST && wp == PBT_APMRESUMEAUTOMATIC) {
        // 系统从睡眠/休眠中恢复唤醒，主循环检测此标志后显示打招呼气泡
        g_powerResumed = true;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}


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
    State state       = State::Normal; // 当前桌宠状态，初始为正常状态
    TP    stateEnd    = {};            // 点击状态（Smile/Laugh）的到期时间点
    bool  prevBusy    = false;         // 上一帧的 isBusy() 结果，用于检测上升沿触发 PreCry
    TP    preCryStart = {};            // PreCry 状态的开始时间点

    // 系统监控器和物理系统实例
    SystemMonitor sysmon;
    Physics       phys;

    // ---- 帧动画：加载真实素材 ----
    // 加载失败不崩溃，loaded=false 时渲染阶段回退到占位图形
    Animation idleAnim, smileAnim, laughAnim, cryAnim;
    idleAnim .load("assets/idle",  "idle",  58);
    smileAnim.load("assets/smile", "smile",  9);
    laughAnim.load("assets/laugh", "laugh", 68);
    cryAnim  .load("assets/cry",   "cry",   39); // cry 动画：39帧，循环播放
    smileAnim.loop = false; // smile 只播一遍，与状态持续时间（750ms）严格对齐
    laughAnim.loop = false; // laugh 只播一遍，与状态持续时间（5667ms）严格对齐
    // cryAnim.loop 默认 true，持续循环直至 CPU/内存负载降低

    // 根据 idle 第一帧尺寸自适应窗口大小，并同步更新物理边界参数
    if (idleAnim.loaded) {
        auto sz = idleAnim.frames[0].getSize();
        window.setSize(sz);
        window.setView(sf::View(sf::FloatRect(
            {0.f, 0.f},
            {static_cast<float>(sz.x), static_cast<float>(sz.y)})));
        phys.winWidth  = static_cast<int>(sz.x);
        phys.winHeight = static_cast<int>(sz.y);
        // 重新置顶，setSize 可能短暂打乱层叠顺序
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        // 以 laugh 第一帧为基准，统一校准三套动画的 displayScale：
        // laugh 按 TARGET_W 等比缩放（宽恒为 TARGET_W，高按原始宽高比）；
        // idle 和 smile 的 X/Y 缩放分别对齐到 laugh 的渲染尺寸，使三者视觉大小完全一致。
        if (laughAnim.loaded) {
            auto lsz     = laughAnim.frames[0].getSize();
            float lscale = TARGET_W / static_cast<float>(lsz.x);   // laugh 等比缩放系数
            laughAnim.displayScale = {lscale, lscale};

            // laugh 渲染后的实际像素尺寸（宽固定为 TARGET_W，高按宽高比）
            float rendW = TARGET_W;
            float rendH = lscale * static_cast<float>(lsz.y);

            // idle 和 smile：X/Y 分别缩放，使渲染宽高与 laugh 完全对齐
            auto alignToLaugh = [&](Animation& anim) {
                if (!anim.loaded || anim.frames.empty()) return;
                auto fsz = anim.frames[0].getSize();
                anim.displayScale = {
                    rendW / static_cast<float>(fsz.x),
                    rendH / static_cast<float>(fsz.y)
                };
            };
            alignToLaugh(idleAnim);
            alignToLaugh(smileAnim);
            alignToLaugh(cryAnim);

            // displayScale 确定后，预算碰撞检测用的身体矩形偏移
            phys.setupBodyRect(idleAnim.displayScale.x, idleAnim.displayScale.y);
        }
    }

    // ---- 疲惫状态：连续使用60分钟后触发，程序重启才恢复 ----
    // SFML 3 已移除内置字体，使用 Windows 系统自带的 Arial 字体（无需随程序分发）
    sf::Font bubbleFont;
    bool     fontLoaded = bubbleFont.openFromFile("C:/Windows/Fonts/arial.ttf");
    TP       startTime  = Clock::now(); // 程序启动时间：供疲惫倒计时和陪伴时长两用
    bool     tired      = false;        // 疲惫标志：一旦置 true 不再重置
    // "刷新至正常状态"强制标志：持续30秒内跳过疲惫/CPU/内存检测，强制保持 Normal
    bool     forceNormal      = false;
    TP       forceNormalUntil = {};

    // ---- 累计陪伴时长（跨 session 持久化到 nailong_time.txt）----
    // 文件存储历史累计分钟数；退出时追加本次增量并写回
    int64_t baseMinutes = 0;
    {
        std::ifstream fin("nailong_time.txt");
        if (fin) fin >> baseMinutes;
    }

    // ---- 音效系统：sf::Music 流式播放 MP3，加载失败回退正弦波 SoundBuffer ----
    // Music 走独立解码路径，对 MP3 兼容性优于 SoundBuffer::loadFromFile
    sf::Music smileMusic, laughMusic;
    bool smileMusicOk = smileMusic.openFromFile("assets/sounds/smile.mp3");
    bool laughMusicOk = laughMusic.openFromFile("assets/sounds/laugh.mp3");
    smileMusic.setLooping(false);
    laughMusic.setLooping(false);

    // 回退用 SoundBuffer（仅 Music 加载失败时启用）
    // 缓冲区须在 Sound 之前声明，生命周期不短于 Sound 对象
    sf::SoundBuffer smileFallBuf, laughFallBuf;
    if (!smileMusicOk) smileFallBuf = makeSineBuffer(660.f, 0.15f);
    if (!laughMusicOk) laughFallBuf = makeSineBuffer(880.f, 0.30f);
    sf::Sound smileFallSound(smileFallBuf), laughFallSound(laughFallBuf);
    smileFallSound.setVolume(70.f);
    laughFallSound.setVolume(70.f);

    // 统一播放接口：优先 Music，降级 SoundBuffer；stop() 防止重叠播放
    auto playSmile = [&] {
        if (smileMusicOk) { smileMusic.stop(); smileMusic.play(); }
        else smileFallSound.play();
    };
    auto playLaugh = [&] {
        if (laughMusicOk) { laughMusic.stop(); laughMusic.play(); }
        else laughFallSound.play();
    };
    bool isMuted       = false; // 静音标志，默认开启；右键菜单切换
    bool isPinned      = false; // 固定标志：true 时禁止拖动和甩动
    bool laughDisabled = false; // 禁用点击动画：true 时点击不触发 Smile/Laugh

    // ---- 点击计数相关变量 ----
    std::deque<TP> clicks;          // 记录最近点击的时间戳，用于检测快速连击
    constexpr int  LAUGH_N  = 5;    // 触发大笑所需的连击次数
    constexpr int  RAPID_MS = 1500; // 连击有效时间窗口（1.5秒内的点击才算连击）

    // laugh 音效延迟播放：进入大笑状态后等 0.3 秒再播放，避免音画不同步
    bool laughSoundPending = false; // 是否有待播放的 laugh 音效
    TP   laughSoundAt      = {};    // 预定播放的时间点

    // ---- 鼠标拖动相关变量 ----
    bool         dragging = false;  // 鼠标左键是否正在按住
    bool         dragged  = false;  // 本次按住期间是否发生了有效位移（区分点击与拖动）
    sf::Vector2i pressPos {};       // 鼠标按下时的屏幕坐标（用于计算位移和判断拖动）
    sf::Vector2i dragOff  {};       // 鼠标按下时，鼠标相对于窗口左上角的偏移量

    // ---- 甩动速度历史记录 ----
    // 记录拖动过程中的位置和时间，松开时用最近100ms内的轨迹计算初速度
    struct PosStamp { sf::Vector2i pos; TP t; };
    std::deque<PosStamp> posHistory;

    // ---- 系统托盘 ----
    // 注册并创建独立的隐藏消息窗口（HWND_MESSAGE），专门接收托盘回调。
    // 使用 HWND_MESSAGE 的好处：该窗口没有桌面呈现，不出现在 Alt+Tab，
    // 且其消息队列完全独立于 SFML 窗口，PeekMessage 不会互相干扰。
    WNDCLASSW trayClass{};
    trayClass.lpfnWndProc   = TrayWndProc;
    trayClass.hInstance     = GetModuleHandle(nullptr);
    trayClass.lpszClassName = L"NailongTrayMsg";
    RegisterClassW(&trayClass);
    HWND trayWnd = CreateWindowW(
        L"NailongTrayMsg", nullptr, 0,
        0, 0, 0, 0,
        HWND_MESSAGE,               // 消息专用窗口，不可见
        nullptr, GetModuleHandle(nullptr), nullptr);

    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = trayWnd;      // 托盘消息发往独立窗口，不经过 SFML
    nid.uID              = 1;            // 本程序只有一个托盘图标，固定 ID=1
    nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP + 1;  // 鼠标操作托盘图标时发送到 trayWnd 的消息号
    nid.hIcon            = LoadIcon(nullptr, IDI_APPLICATION); // 暂用系统默认图标
    wcscpy_s(nid.szTip, L"奶龙桌面宠物");
    bool trayAdded = false; // 托盘图标是否已添加（true 时窗口处于隐藏状态）

    // ---- 注册系统休眠/唤醒通知（发往独立消息窗口 trayWnd）----
    // RegisterSuspendResumeNotification 发送有针对性的 WM_POWERBROADCAST，
    // 不是广播消息，HWND_MESSAGE 窗口可以正常接收。
    HPOWERNOTIFY hPowerNotify = RegisterSuspendResumeNotification(
        trayWnd, DEVICE_NOTIFY_WINDOW_HANDLE);

    // ---- 启动 IP 属地查询子线程 ----
    std::thread(fetchIpLocation).detach();

    // ---- 打招呼气泡：程序启动时立即显示 ----
    GreetBubble greet;
    auto buildGreetLines = [&](TP t) -> std::vector<std::wstring> {
        int64_t mins = baseMinutes + std::chrono::duration_cast<std::chrono::minutes>(
                           t - startTime).count();
        std::string ipSnap;
        { std::lock_guard<std::mutex> lk(s_ipMutex); ipSnap = s_ipLocation; }
        return {
            getGreeting(),
            getDateStr(),
            std::wstring(L"Companion time: ") + std::to_wstring(mins) + L" mins",
            std::wstring(L"Location: ") + utf8ToWide(ipSnap)
        };
    };
    greet.show(buildGreetLines(Clock::now()), Clock::now());

    // ============================================================
    // 主循环：事件处理 → 状态更新 → 渲染，每帧执行一次
    // ============================================================
    TP prevFrameTime = Clock::now(); // 上一帧时间点，用于计算 deltaTime
    while (window.isOpen()) {
        TP  now = Clock::now(); // 记录本帧开始时间，供本帧所有逻辑统一使用
        // 帧时间差，最大限制 100ms，防止调试暂停后动画跳帧
        float dt = std::min(
            std::chrono::duration_cast<Fsec>(now - prevFrameTime).count(), 0.1f);
        prevFrameTime = now;

        // ---- 托盘消息处理 ----
        // 用 PeekMessage + DispatchMessage 驱动独立消息窗口（trayWnd）的消息队列，
        // DispatchMessage 将消息路由给 TrayWndProc，后者写入 g_restoreFromTray。
        // 此处理与 SFML 的 pollEvent 完全独立，不存在消息被 SFML 吞噬的问题。
        {
            MSG m;
            while (PeekMessageW(&m, trayWnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m); // → TrayWndProc → 设置 g_restoreFromTray
            }
            if (g_restoreFromTray && trayAdded) {
                g_restoreFromTray = false;
                window.setVisible(true);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                Shell_NotifyIconW(NIM_DELETE, &nid);
                trayAdded = false;
            }
        }

        sysmon.sample();       // 尝试采样系统资源（内部节流，2秒最多一次）

        // ---- 电脑唤醒检测 ----
        // g_powerResumed 由 TrayWndProc 在收到 WM_POWERBROADCAST(PBT_APMRESUMEAUTOMATIC) 时设置
        if (g_powerResumed) {
            g_powerResumed = false;
            // 若窗口已最小化到托盘，先恢复显示
            if (trayAdded) {
                window.setVisible(true);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                Shell_NotifyIconW(NIM_DELETE, &nid);
                trayAdded = false;
            }
            // 唤醒后重新查询 IP（可能已切换网络），重置查询状态
            { std::lock_guard<std::mutex> lk(s_ipMutex); s_ipLocation = "loading..."; }
            s_ipReady = false;
            std::thread(fetchIpLocation).detach();
            // 显示唤醒打招呼气泡
            greet.show(buildGreetLines(now), now);
        }

        greet.update(now); // 更新气泡：检查超时，IP 查询完成后刷新最后一行

        // forceNormal 到期检查：30秒后自动恢复正常状态机逻辑
        if (forceNormal && now >= forceNormalUntil)
            forceNormal = false;

        // forceNormal 期间跳过疲惫检测，避免计时被"偷走"
        if (!tired && !forceNormal) {
            auto usedMs = std::chrono::duration_cast<Ms>(now - startTime).count();
            if (usedMs >= 60LL * 60 * 1000)
                tired = true;
        }

        // 记录本帧 isBusy() 结果：既用于上升沿检测，也用于 baseState 计算
        // forceNormal 期间将 curBusy 视为 false，阻止 Cry/PreCry 触发
        bool curBusy = !forceNormal && sysmon.isBusy();

        // 计算本帧的"基础状态"：飞行/拖动/点击等高优先级状态结束后的默认回落目标
        // 优先级：疲惫 > PreCry > CPU/内存过载（哭泣）> 正常
        // forceNormal 期间忽略 tired 和 curBusy，强制回落到 Normal
        State baseState = (!forceNormal && tired) ? State::Tired :
                          curBusy                 ? State::Cry   :
                                                    State::Normal;

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
                }  // end Left button

            // 鼠标移动事件
            } else if (ev->is<sf::Event::MouseMoved>()) {
                // isPinned 时跳过整个拖动逻辑：窗口不移动，dragged 不置 true，
                // 这样 wasClick 仍能正确判定为点击，Smile/Laugh 不受影响
                if (dragging && !isPinned) {
                    sf::Vector2i cur = sf::Mouse::getPosition(); // 当前鼠标屏幕坐标
                    auto d = cur - pressPos;
                    // 位移超过5像素才判定为拖动（避免手抖被误判）
                    if (std::abs(d.x) > 5 || std::abs(d.y) > 5)
                        dragged = true;
                    // 将窗口移动到"当前鼠标位置 - 初始偏移"，实现平滑跟手
                    window.setPosition(cur - dragOff);

                    // 记录当前位置和时间，保留最近 200ms 的轨迹
                    // 200ms 分两段：早段(200~100ms)用于判断加速趋势，晚段(100~0ms)用于计算初速度
                    posHistory.push_back({cur, now});
                    while (posHistory.size() > 1) {
                        float age = std::chrono::duration_cast<Fsec>(
                                        now - posHistory.front().t).count();
                        if (age > 0.20f) posHistory.pop_front(); // 淘汰 200ms 前的记录
                        else             break;
                    }
                }

            // 鼠标松开事件
            } else if (auto* e = ev->getIf<sf::Event::MouseButtonReleased>()) {
                if (e->button == sf::Mouse::Button::Left) {
                    // wasClick：按下后没有发生有效位移，判定为点击而非拖动
                    bool wasClick = dragging && !dragged;
                    dragging = false;

                    // ---- 甩动判定（速度趋势方案）----
                    // 核心思想：将拖动轨迹分为"早段"(200~100ms前)和"晚段"(100ms~松手)两段，
                    // 比较两段的平均速度大小，判断用户是在加速甩出还是减速放置。
                    // 加速甩出时，对速度要求低；减速放置时，要求很高速才触发（几乎不触发）。
                    if (!isPinned && dragged && posHistory.size() >= 2) {
                        auto& last    = posHistory.back();
                        TP    refTime = last.t; // 以最后一条记录的时间作为"松手时刻"基准

                        // 总位移：从按下点到松开点的直线距离（像素）
                        float totalDisp = std::hypot(
                            (float)(last.pos.x - pressPos.x),
                            (float)(last.pos.y - pressPos.y));

                        // 遍历历史，将各记录按时间归入早段或晚段
                        // 早段：距离松手 100ms~200ms 的记录
                        // 晚段：距离松手 0ms~100ms 的记录
                        bool         hasEarly = false,      hasLate = false;
                        sf::Vector2i earlyFirstPos = {},    earlyLastPos = {};
                        sf::Vector2i lateFirstPos  = {},    lateLastPos  = {};
                        TP           earlyFirstT   = {},    earlyLastT   = {};
                        TP           lateFirstT    = {},    lateLastT    = {};

                        for (const auto& ps : posHistory) {
                            float age = std::chrono::duration_cast<Fsec>(
                                            refTime - ps.t).count();
                            if (age > 0.10f && age <= 0.20f) {
                                // 早段：第一条记录作为起点，后续不断更新终点
                                if (!hasEarly) { earlyFirstPos = ps.pos; earlyFirstT = ps.t; }
                                earlyLastPos = ps.pos; earlyLastT = ps.t;
                                hasEarly = true;
                            } else if (age >= 0.f && age <= 0.10f) {
                                // 晚段：同上
                                if (!hasLate) { lateFirstPos = ps.pos; lateFirstT = ps.t; }
                                lateLastPos = ps.pos; lateLastT = ps.t;
                                hasLate = true;
                            }
                        }

                        // 计算晚段平均速度 velLate（像素/秒），用于甩动初速度
                        sf::Vector2f velLate = {};
                        float dtLate = std::chrono::duration_cast<Fsec>(
                                           lateLastT - lateFirstT).count();
                        if (hasLate && dtLate > 0.001f) {
                            velLate = {
                                (float)(lateLastPos.x - lateFirstPos.x) / dtLate,
                                (float)(lateLastPos.y - lateFirstPos.y) / dtLate
                            };
                        }

                        // 计算早段平均速度 velEarly（像素/秒），用于判断加速趋势
                        sf::Vector2f velEarly = {};
                        float dtEarly = std::chrono::duration_cast<Fsec>(
                                            earlyLastT - earlyFirstT).count();
                        if (hasEarly && dtEarly > 0.001f) {
                            velEarly = {
                                (float)(earlyLastPos.x - earlyFirstPos.x) / dtEarly,
                                (float)(earlyLastPos.y - earlyFirstPos.y) / dtEarly
                            };
                        }

                        float speedLate  = std::hypot(velLate.x,  velLate.y);
                        float speedEarly = std::hypot(velEarly.x, velEarly.y);
                        // trend > 0：晚段比早段快（加速趋势，典型甩动）
                        // trend ≤ -100：晚段比早段慢很多（减速趋势，典型放置）
                        float trend = speedLate - speedEarly;

                        // 只有"松手前明确加速"才触发甩动，减速或匀速一律视为拖动放置
                        // 条件：trend > 30（晚段比早段快30px/s以上）且 晚段速度本身 > 80px/s
                        if (trend > 30.f && speedLate > 150.f && totalDisp > 80.f)
                            // 用晚段速度作为初速度，从像素/秒换算为像素/帧（÷60）
                            phys.launch({velLate.x / 60.f, velLate.y / 60.f});
                    }
                    posHistory.clear(); // 松开后清空历史，节省内存

                    // ---- 点击计数逻辑（仅在判定为点击时执行）----
                    // 处于大笑状态、或 laughDisabled 时忽略点击
                    if (wasClick && state != State::Laugh && !laughDisabled) {
                        clicks.push_back(now); // 记录本次点击时间

                        // 淘汰1.5秒以前的旧点击记录（超出连击时间窗口）
                        while (!clicks.empty()) {
                            auto age = std::chrono::duration_cast<Ms>(
                                           now - clicks.front()).count();
                            if (age > RAPID_MS) clicks.pop_front();
                            else                break;
                        }

                        // 连击5次 → 触发大笑状态，持续 68帧×(1/12s)≈5667ms，与动画时长严格对齐
                        if (static_cast<int>(clicks.size()) >= LAUGH_N) {
                            state    = State::Laugh;
                            stateEnd = now + Ms(5667);
                            laughAnim.reset(); // 从第一帧开始播
                            clicks.clear(); // 清空计数，下一轮重新累计
                            // 延迟 0.3 秒播放音效，登记时间点，由主循环统一检测触发
                            if (!isMuted) {
                                laughSoundPending = true;
                                laughSoundAt      = now + Ms(300);
                            }
                        } else {
                            // 普通点击 → 触发微笑状态，持续 9帧×(1/12s)=750ms，与动画时长严格对齐
                            state    = State::Smile;
                            stateEnd = now + Ms(750);
                            smileAnim.reset(); // 从第一帧开始播，避免接力残留帧
                            // 清除残留的挤压动画，防止 squish scale 叠加到 smile 渲染上
                            phys.squish = {};
                            if (!isMuted) playSmile();
                        }
                    }

                } else if (e->button == sf::Mouse::Button::Right) {
                    // ---- 右键松开：弹出原生右键菜单 ----
                    // 菜单项文字根据当前状态动态切换
                    // 构建顶部信息字符串（整数取整，%% 输出字面量 %）
                    wchar_t sysInfo[64];
                    swprintf_s(sysInfo, L"CPU: %d%%  内存: %d%%",
                               static_cast<int>(sysmon.cpuUsage),
                               static_cast<int>(sysmon.memUsage));

                    HMENU menu = CreatePopupMenu();
                    // 信息项：纯展示，MF_GRAYED 使其变灰且不可点击
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, sysInfo);

                    // 陪伴时长信息项
                    wchar_t companionInfo[64];
                    int64_t menuMins = baseMinutes +
                        std::chrono::duration_cast<std::chrono::minutes>(
                            now - startTime).count();
                    swprintf_s(companionInfo, L"奶龙已陪伴你 %lld 分钟",
                               static_cast<long long>(menuMins));
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, companionInfo);

                    // IP属地信息项
                    wchar_t ipInfo[256];
                    if (!s_ipReady.load()) {
                        wcscpy_s(ipInfo, L"IP属地：查询中...");
                    } else {
                        std::lock_guard<std::mutex> lk(s_ipMutex);
                        if (s_ipLocation == "unknown")
                            wcscpy_s(ipInfo, L"IP属地：未知");
                        else
                            swprintf_s(ipInfo, L"IP属地：%ls",
                                       utf8ToWide(s_ipLocation).c_str());
                    }
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, ipInfo);

                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING,    1, L"关闭");
                    AppendMenuW(menu, MF_STRING,    2, L"最小化到托盘");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING,    3,
                        isPinned      ? L"取消固定"       : L"固定");
                    AppendMenuW(menu, MF_STRING,    4,
                        laughDisabled ? L"取消禁用Laughing" : L"禁用Laughing");
                    AppendMenuW(menu, MF_STRING,    5,
                        isMuted       ? L"取消静音"       : L"静音");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING,    6, L"刷新至正常状态");

                    POINT pt;
                    GetCursorPos(&pt);
                    // SetForegroundWindow 是 TrackPopupMenu 正确消失的前置条件：
                    // 只有前台窗口的菜单才会在点击外部时自动关闭
                    SetForegroundWindow(hwnd);
                    int cmd = TrackPopupMenu(menu,
                                            TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    // TrackPopupMenu 标准收尾：发一条 WM_NULL 刷新消息队列
                    PostMessage(hwnd, WM_NULL, 0, 0);

                    switch (cmd) {
                    case 1: // 关闭
                        window.close();
                        break;
                    case 2: // 最小化到托盘：隐藏 SFML 窗口并添加托盘图标
                        if (!trayAdded) {
                            Shell_NotifyIconW(NIM_ADD, &nid);
                            trayAdded = true;
                        }
                        window.setVisible(false);
                        break;
                    case 3: isPinned      = !isPinned;      break;
                    case 4: laughDisabled = !laughDisabled; break;
                    case 5: isMuted       = !isMuted;       break;
                    case 6: // 刷新至正常状态：重置所有临时状态，不影响开关类设置
                        state             = State::Normal;
                        stateEnd          = {};          // 清除 Smile/Laugh 计时
                        tired             = false;       // 解除疲惫标志
                        startTime         = now;         // 重置疲惫倒计时起点
                        clicks.clear();                  // 清空连击计数
                        greet.active      = false;       // 关闭打招呼气泡
                        forceNormal       = true;        // 30秒内屏蔽疲惫/CPU/内存检测
                        forceNormalUntil  = now + std::chrono::seconds(30);
                        break;
                    }
                }
            }
        }

        // ============================================================
        // 状态优先级判定（从高到低）：
        //   飞行/弹跳 > 拖动 > 点击(微笑/大笑) > 疲惫 > PreCry > 哭泣 > 正常
        // 各高优先级状态结束后统一回落到 baseState（由疲惫/CPU负载决定）
        // ============================================================

        if (phys.active) {
            // 最高优先级：飞行状态 —— 每帧推进物理模拟
            phys.update(window, now);
            // 物理本帧刚停止：立即在同帧内完成状态转换，避免渲染阶段读到过时的旧状态而闪烁
            if (!phys.active)
                state = baseState;

        } else if (dragging && dragged) {
            // 拖动状态：鼠标按住且发生了有效位移
            state = State::Drag;

        } else if (state == State::Drag) {
            // 拖动刚结束：回落到 baseState（可能是 Tired / Cry / Normal）
            state = baseState;

        } else if (state == State::Smile || state == State::Laugh) {
            // 点击状态计时到期：回落到 baseState
            if (now >= stateEnd) {
                state = baseState;
            }

        } else if (state == State::PreCry) {
            // PreCry 过渡状态：播放1秒后自动切换到 Cry
            if (std::chrono::duration_cast<Ms>(now - preCryStart).count() >= 1000)
                state = State::Cry;

        } else {
            // 默认情况：始终跟随 baseState
            // 检测 isBusy 上升沿（false → true）：非疲惫且非强制正常时才触发 PreCry
            if (!prevBusy && curBusy && !tired && !forceNormal) {
                state       = State::PreCry;
                preCryStart = now;
            } else {
                state = baseState;
            }
        }

        // 记录本帧 isBusy 结果，供下一帧检测上升沿（刚超阈值那一刻）
        prevBusy = curBusy;

        // ---- laugh 音效延迟触发：到达预定时间点才播放 ----
        if (laughSoundPending && now >= laughSoundAt) {
            laughSoundPending = false;
            playLaugh();
        }

        // ---- 更新帧动画（三套动画同时推进，各自帧率独立） ----
        idleAnim .update(dt);
        smileAnim.update(dt);
        laughAnim.update(dt);
        cryAnim  .update(dt);

        // ============================================================
        // 渲染：根据当前状态绘制对应形状和颜色
        // 黑色背景会被 LWA_COLORKEY 设为透明，只有彩色部分可见
        // ============================================================
        window.clear(sf::Color::Black);

        // 每帧获取当前窗口尺寸（自适应素材分辨率）
        auto  winSize = window.getSize();
        float ww = static_cast<float>(winSize.x);
        float wh = static_cast<float>(winSize.y);

        if (phys.active) {
            // 飞行/弹跳状态：idle 动画 + 挤压缩放；无素材时回退橙色圆形
            sf::Vector2f squishScale = phys.squish.getScale(now);
            if (idleAnim.loaded) {
                window.draw(idleAnim.getCurrentSprite(winSize, squishScale));
            } else {
                constexpr float r = 80.f;
                sf::CircleShape circle(r);
                circle.setFillColor(sf::Color(255, 140, 0));
                circle.setScale(squishScale);
                circle.setPosition({ww / 2.f - r * squishScale.x,
                                    wh / 2.f - r * squishScale.y});
                window.draw(circle);
            }

        } else if (state == State::Drag) {
            // 拖动状态：播放 idle 动画（与静止外观一致，体现被抓起但未甩动）
            if (idleAnim.loaded)
                window.draw(idleAnim.getCurrentSprite(winSize));

        } else if (state == State::Smile) {
            // 微笑状态：smile 动画；无素材时回退黄色圆形
            if (smileAnim.loaded) {
                window.draw(smileAnim.getCurrentSprite(winSize));
            } else {              
                constexpr float r = 90.f;
                sf::CircleShape circle(r);
                circle.setFillColor(sf::Color(255, 220, 0));
                circle.setPosition({ww / 2.f - r, wh / 2.f - r});
                window.draw(circle);
            }

        } else if (state == State::Laugh) {
            // 大笑状态：laugh 动画；无素材时回退红色圆形
            if (laughAnim.loaded) {
                window.draw(laughAnim.getCurrentSprite(winSize));
            } else {
                constexpr float r = 110.f;
                sf::CircleShape circle(r);
                circle.setFillColor(sf::Color(220, 50, 50));
                circle.setPosition({ww / 2.f - r, wh / 2.f - r});
                window.draw(circle);
            }

        } else if (state == State::Cry) {
            // 哭泣状态：cry 动画循环播放；无素材时回退蓝色圆形
            if (cryAnim.loaded) {
                window.draw(cryAnim.getCurrentSprite(winSize));
            } else {
                constexpr float r = 60.f;
                sf::CircleShape circle(r);
                circle.setFillColor(sf::Color(50, 100, 255));
                circle.setPosition({ww / 2.f - r, wh / 2.f - r});
                window.draw(circle);
            }

        } else {
            // Normal / PreCry / Tired：统一播放 idle 动画；无素材时按状态回退色块
            if (idleAnim.loaded) {
                window.draw(idleAnim.getCurrentSprite(winSize));
            } else {
                float r = 80.f;
                sf::Color c = sf::Color(255, 140, 0);
                if (state == State::Tired) { r = 65.f; c = sf::Color(160, 160, 160); }
                sf::CircleShape circle(r);
                circle.setFillColor(c);
                circle.setPosition({ww / 2.f - r, wh / 2.f - r});
                window.draw(circle);
            }

            // 疲惫状态：在动画上叠加"Take a break!"气泡
            if (state == State::Tired) {
                constexpr float bubbleW = 130.f, bubbleH = 38.f;
                // 气泡底边对齐到奶龙身体顶部上方 10px
                float bx = ww / 2.f + 10.f;
                float by = static_cast<float>(phys.bodyOffT) - 10.f - bubbleH;
                sf::RectangleShape bubble({bubbleW, bubbleH});
                bubble.setFillColor(sf::Color(255, 255, 255));
                bubble.setOutlineColor(sf::Color(140, 140, 140));
                bubble.setOutlineThickness(2.f);
                bubble.setPosition({bx, by});
                window.draw(bubble);
                if (fontLoaded) {
                    sf::Text text(bubbleFont, "Take a break!", 14);
                    text.setFillColor(sf::Color(70, 70, 70));
                    auto b = text.getLocalBounds();
                    text.setOrigin({b.position.x + b.size.x / 2.f,
                                    b.position.y + b.size.y / 2.f});
                    text.setPosition({bx + 65.f, by + 19.f});
                    window.draw(text);
                }
            }
        }

        // 打招呼气泡：最后绘制，叠加在宠物形象上方，20秒后自动消失
        if (greet.active && fontLoaded) {
            constexpr float bx   = 5.f;
            float bw = std::max(200.f, ww - 10.f);
            constexpr float lineH = 22.f; // 行间距，与字号 15 匹配
            float bh = 10.f + static_cast<float>(greet.lines.size()) * lineH;
            // 气泡底边对齐到奶龙身体顶部上方 10px
            float by = static_cast<float>(phys.bodyOffT) - 10.f - bh;

            sf::RectangleShape bg({bw, bh});
            bg.setFillColor(sf::Color(255, 255, 255));
            bg.setOutlineColor(sf::Color(180, 180, 180));
            bg.setOutlineThickness(1.5f);
            bg.setPosition({bx, by});
            window.draw(bg);

            for (std::size_t i = 0; i < greet.lines.size(); ++i) {
                sf::Text ln(bubbleFont, sf::String(greet.lines[i]), 15);
                ln.setFillColor(sf::Color(40, 40, 40));
                ln.setPosition({bx + 8.f,
                                by + 5.f + static_cast<float>(i) * lineH});
                window.draw(ln);
            }
        }

        window.display(); // 将缓冲区内容提交到屏幕
    }

    // 退出时将本次运行时长累加并写回持久化文件
    {
        int64_t total = baseMinutes + std::chrono::duration_cast<std::chrono::minutes>(
                            Clock::now() - startTime).count();
        std::ofstream fout("nailong_time.txt");
        if (fout) fout << total;
    }

    // 注销电源通知，清除托盘图标和消息窗口
    if (hPowerNotify)
        UnregisterSuspendResumeNotification(hPowerNotify);
    if (trayAdded)
        Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(trayWnd);
    UnregisterClassW(L"NailongTrayMsg", GetModuleHandle(nullptr));

    return 0;
}
