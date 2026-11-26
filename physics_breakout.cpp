// ============================================================================
//  EasyX 2D 物理弹球闯关游戏  ——  C++ 课程作业
// ----------------------------------------------------------------------------
//  编译（Visual Studio 2022/2026 + EasyX 26.x）：
//      cl /EHsc /utf-8 /DUNICODE /D_UNICODE physics_breakout.cpp ^
//         /I "<EasyX目录>\include" /link "<EasyX目录>\lib_vc2015\x64\EasyXw.lib"
//  也可以在 VS 里：项目属性 -> VC++ 目录 -> 包含目录/库目录 指向 EasyX 的
//  include 与 lib_vc2015\x64，然后直接 F5。
//  运行：窗口 800 x 620，鼠标左右移动控制挡板，鼠标左键或空格发球，P 暂停，R 重开。
//
//  报告导读：
//  1) 所有“物理计算”集中在【第 2 节】，函数命名即功能，可直接摘进报告；
//  2) 每个物理函数上方都写了所用公式与变量含义；
//  3) 关卡布局在【第 3 节】用字符串表描述，改关卡不用碰逻辑代码；
//  4) 第 5 节 update() 是固定步长积分器，第 6 节是渲染，逻辑与显示分离。
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <graphics.h>
#include <conio.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <tchar.h>

// ============================================================================
//  第 1 节：常量与数据结构
// ============================================================================

const int  WIN_W = 800, WIN_H = 620;          // 窗口尺寸
const int  FIELD_L = 40, FIELD_R = 760;       // 可运动区域左右边界
const int  FIELD_T = 70, FIELD_B = 585;       // 上下边界（下边界以下算掉落）

const int  BRICK_COLS = 13, BRICK_ROWS = 8;   // 每关砖块网格
const int  BRICK_TOP = 100;                   // 砖块区域顶部
const int  BRICK_H = 24;                      // 单块砖高度
const int  BRICK_GAP = 3;                     // 砖块间隙
const double BRICK_W = (FIELD_R - FIELD_L) / (double)BRICK_COLS;

const int  PADDLE_W = 110, PADDLE_H = 12;      // 挡板尺寸
const int  PADDLE_Y = 550;                     // 挡板顶部 y
const double BALL_R = 7.0;                     // 小球半径

// --- 物理参数（报告里可以列表说明这些量的取值与含义） ---
const double GRAVITY       = 150.0;   // 重力加速度 g，单位 px/s²
const double RESTITUTION   = 0.94;    // 碰撞恢复系数 e：1 完全弹性，0 完全非弹性
const double LAUNCH_SPEED  = 400.0;   // 发球速度 |v0|，px/s
const double MIN_SPEED     = 300.0;   // 速度下限，防止球越弹越慢卡住
const double MAX_SPEED     = 620.0;   // 速度上限，防止穿透砖块
const double FIXED_DT      = 1.0 / 120.0;  // 物理积分步长，固定 120Hz

// 二维向量：游戏的物理量（位置/速度/法线）都用这个结构表示
struct Vec2
{
    double x, y;
};

struct Ball
{
    Vec2 pos;      // 球心坐标
    Vec2 vel;      // 速度，px/s
    bool alive;    // 是否仍在场（掉出下边界后为 false）
};

struct Paddle
{
    double cx;     // 挡板中心 x，直接跟随鼠标
    int    y;      // 挡板顶部 y（固定）
};

struct Brick
{
    int  x, y;     // 左上角
    int  w, h;
    int  hp;       // 剩余耐久：1=普通砖，2/3=硬砖
    int  maxHp;    // 初始耐久，用于上色/显示击打次数
    bool alive;
};

enum GameState { ST_READY, ST_PLAYING, ST_CLEAR, ST_OVER, ST_WIN };

// ============================================================================
//  第 2 节：物理计算函数（报告核心，全部与绘图无关）
// ============================================================================

// --- 2.1 向量基础运算 ---
// 向量加法：a + b = (a.x+b.x, a.y+b.y)
static Vec2 vecAdd(const Vec2& a, const Vec2& b)
{
    return Vec2{ a.x + b.x, a.y + b.y };
}

// 向量减法：a - b，常用于求“球心 - 矩形最近点”得到碰撞法线方向
static Vec2 vecSub(const Vec2& a, const Vec2& b)
{
    return Vec2{ a.x - b.x, a.y - b.y };
}

// 数乘：k * a
static Vec2 vecScale(const Vec2& a, double k)
{
    return Vec2{ a.x * k, a.y * k };
}

// 点积：a·b = ax*bx + ay*by。反射公式和正交判定都要用
static double vecDot(const Vec2& a, const Vec2& b)
{
    return a.x * b.x + a.y * b.y;
}

// 向量长度 |a| = sqrt(ax² + ay²)
static double vecLength(const Vec2& a)
{
    return std::sqrt(a.x * a.x + a.y * a.y);
}

// 单位化：a / |a|。零向量原样返回，避免除 0
static Vec2 vecNormalize(const Vec2& a)
{
    double len = vecLength(a);
    if (len < 1e-9) return Vec2{ 0.0, 0.0 };
    return Vec2{ a.x / len, a.y / len };
}

// 区间限幅：把 v 夹在 [lo, hi]，鼠标位置、速度大小都会用到
static double clampValue(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// --- 2.2 重力与积分 ---
// 半隐式欧拉积分（symplectic Euler）：
//   v(t+dt) = v(t) + a*dt
//   p(t+dt) = p(t) + v(t+dt)*dt
// 相比显式欧拉更稳定，是游戏物理里最常用的积分方式。
// 本函数负责第一步：速度更新。加速度 a = (0, g)，因为 y 轴向下为正。
static void applyGravity(Vec2& vel, double dt)
{
    vel.y += GRAVITY * dt;
}

// 积分第二步：用更新后的速度推进位置
static void integratePosition(Vec2& pos, const Vec2& vel, double dt)
{
    pos.x += vel.x * dt;
    pos.y += vel.y * dt;
}

// --- 2.3 速度限幅 ---
// 把速度大小限制在 [MIN_SPEED, MAX_SPEED]，方向保持不变。
// 必要性：重力会不断加速下落，若不设上限，球可能一帧跨过整块砖（穿透）；
// 而多次非弹性碰撞会让速度衰减到接近 0，球会“黏”在挡板附近。
static void clampSpeed(Vec2& vel)
{
    double speed = vecLength(vel);
    if (speed < 1e-9) return;
    double target = clampValue(speed, MIN_SPEED, MAX_SPEED);
    if (target != speed)
        vel = vecScale(vel, target / speed);
}

// --- 2.4 反射公式 ---
// 已知碰撞单位法线 n（由障碍物指向小球）和恢复系数 e，反射速度为：
//   v' = v - (1 + e) * (v · n) * n
// 推导：把 v 分解为切向分量 vt 和法向分量 vn=(v·n)n；
// 碰撞只把法向分量反向并乘以 e，切向分量保留：v' = vt - e*vn = v-(1+e)vn。
// 仅当 v·n < 0（球正在靠近障碍物）时才反射，避免重叠状态下反复反弹抖动。
static Vec2 reflectVelocity(const Vec2& vel, const Vec2& n, double e)
{
    double vn = vecDot(vel, n);
    if (vn >= 0.0) return vel;
    return vecSub(vel, vecScale(n, (1.0 + e) * vn));
}

// --- 2.5 圆与矩形的碰撞检测 ---
// 求矩形内离圆心最近的点：把圆心坐标分别夹到矩形的 x/y 区间。
static Vec2 closestPointOnRect(const Vec2& p, int x, int y, int w, int h)
{
    return Vec2{ clampValue(p.x, (double)x, (double)(x + w)),
                 clampValue(p.y, (double)y, (double)(y + h)) };
}

// 圆-矩形相交判定：圆心到最近点的距离 < 半径
static bool circleRectTest(const Vec2& center, double r, int x, int y, int w, int h)
{
    Vec2 q = closestPointOnRect(center, x, y, w, h);
    Vec2 d = vecSub(center, q);
    return vecDot(d, d) < r * r;      // 用平方比较，省掉一次 sqrt
}

// --- 2.6 圆与矩形碰撞的完整响应 ---
// 分两步：
//   (1) 位置修正：把球沿法线推出重叠区，推出量 = r - dist，另加 0.5px 余量；
//   (2) 速度响应：调用 reflectVelocity 计算反弹速度。
// 特殊情况：球心已经进入矩形内部（dist≈0），此时最近点就是球心，没有法线，
// 改为比较球心到四条边的距离，选“最浅”的那条边作为逃逸方向。
static bool resolveCircleRect(Ball& ball, int x, int y, int w, int h, double restitution)
{
    if (!circleRectTest(ball.pos, BALL_R, x, y, w, h)) return false;

    Vec2 q = closestPointOnRect(ball.pos, x, y, w, h);
    Vec2 d = vecSub(ball.pos, q);
    double dist = vecLength(d);
    Vec2 n;

    if (dist > 1e-6) {
        n = vecScale(d, 1.0 / dist);                 // 常规情况：法线 = 圆心-最近点
        ball.pos = vecAdd(ball.pos, vecScale(n, BALL_R - dist + 0.5));
    } else {
        // 球心在矩形内部：找最小穿透方向
        double dl = ball.pos.x - x;
        double dr = (x + w) - ball.pos.x;
        double dt = ball.pos.y - y;
        double db = (y + h) - ball.pos.y;
        double m = dl < dr ? dl : dr;
        if (dt < m) m = dt;
        if (db < m) m = db;
        if (m == dl)      n = Vec2{ -1, 0 };
        else if (m == dr) n = Vec2{  1, 0 };
        else if (m == dt) n = Vec2{  0,-1 };
        else              n = Vec2{  0, 1 };
        ball.pos = vecAdd(ball.pos, vecScale(n, m + BALL_R + 0.5));
    }

    ball.vel = reflectVelocity(ball.vel, n, restitution);
    return true;
}

// --- 2.7 挡板反弹（本游戏的核心手感） ---
// 不能简单按反射公式处理挡板，否则重力会让球越弹越低。
// 采用“角度可控 + 能量补充”模型：
//   s = clamp((ball.x - paddle.cx) / (PADDLE_W/2), -1, 1)
//   dir = normalize( (0.85*s, -1) )        // 永远朝上，打挡板中心附近垂直弹起
//   speed = clamp(|v| * 1.03, LAUNCH_SPEED, MAX_SPEED)
//   v' = dir * speed
// 意义：击中挡板位置决定出射角，保留约 3% 动能补偿重力损耗，保证球能回到砖区。
static void bounceOffPaddle(Ball& ball, const Paddle& paddle)
{
    double s = clampValue((ball.pos.x - paddle.cx) / (PADDLE_W * 0.5), -1.0, 1.0);
    Vec2 dir = vecNormalize(Vec2{ 0.85 * s, -1.0 });
    double speed = clampValue(vecLength(ball.vel) * 1.03, LAUNCH_SPEED, MAX_SPEED);
    ball.vel = vecScale(dir, speed);
    ball.pos.y = paddle.y - BALL_R - 0.5;     // 贴到挡板上方，防止连续触发
}

// --- 2.8 场地边界碰撞 ---
// 左右墙、天花板：法线分别为 (1,0)、(-1,0)、(0,1)，用统一反射公式处理。
// 下边界不做反弹，交给主循环判定“掉球扣命”。
static void handleWalls(Ball& ball)
{
    if (ball.pos.x - BALL_R < FIELD_L) {
        ball.pos.x = FIELD_L + BALL_R;
        ball.vel = reflectVelocity(ball.vel, Vec2{ 1, 0 }, RESTITUTION);
    }
    if (ball.pos.x + BALL_R > FIELD_R) {
        ball.pos.x = FIELD_R - BALL_R;
        ball.vel = reflectVelocity(ball.vel, Vec2{ -1, 0 }, RESTITUTION);
    }
    if (ball.pos.y - BALL_R < FIELD_T) {
        ball.pos.y = FIELD_T + BALL_R;
        ball.vel = reflectVelocity(ball.vel, Vec2{ 0, 1 }, RESTITUTION);
    }
}

// ============================================================================
//  第 3 节：关卡数据（字符串描述，改布局只改这里）
//  字符含义：'.' 空   'a' 普通砖(1血)   'b' 硬砖(2血)   'c' 硬砖(3血)
// ============================================================================

static const char* LEVELS[][BRICK_ROWS] = {
    {   // 第 1 关：入门，三排普通砖
        ".............",
        "..aaaaaaaaa..",
        "..aaaaaaaaa..",
        "..aaaaaaaaa..",
        ".............",
        ".............",
        ".............",
        "............."
    },
    {   // 第 2 关：金字塔，中间夹一排硬砖
        ".....aaa.....",
        "....aaaaa....",
        "...aaaaaaa...",
        "..aabbbbaa...",
        "..aaaaaaaaa..",
        ".............",
        ".............",
        "............."
    },
    {   // 第 3 关：棋盘格，普通砖与硬砖交错
        "a.a.a.a.a.a.a",
        ".b.b.b.b.b.b.",
        "a.a.a.a.a.a.a",
        ".b.b.b.b.b.b.",
        "a.a.a.a.a.a.a",
        ".............",
        ".............",
        "............."
    },
    {   // 第 4 关：堡垒，外壳 3 血硬砖
        "..ccccccccc..",
        "..caaaaaaac..",
        "..caaaaaabc..",
        "..caabbaaac..",
        "..caaaaaabc..",
        "..caaaaaaac..",
        "..ccccccccc..",
        "............."
    }
};
const int LEVEL_COUNT = sizeof(LEVELS) / sizeof(LEVELS[0]);

// ============================================================================
//  第 4 节：全局游戏状态
// ============================================================================

static Ball   ball;
static Paddle paddle;
static Brick  bricks[BRICK_COLS * BRICK_ROWS];
static int    brickCount = 0;

static int    score = 0, lives = 3, level = 1;
static int    state = ST_READY;
static double stateTimer = 0.0;        // 通关/失败画面的停留时间
static int    gMouseX = WIN_W / 2;     // 由 EasyX 消息循环更新的鼠标横坐标

// 把字符串关卡加载成砖块数组
static void loadLevel(int lv)
{
    brickCount = 0;
    double bw = BRICK_W - BRICK_GAP;

    for (int r = 0; r < BRICK_ROWS; ++r) {
        const char* row = LEVELS[(lv - 1) % LEVEL_COUNT][r];
        for (int c = 0; c < BRICK_COLS; ++c) {
            char ch = row[c];
            if (ch == '.') continue;
            Brick& b = bricks[brickCount++];
            b.x = (int)(FIELD_L + c * BRICK_W + BRICK_GAP * 0.5);
            b.y = BRICK_TOP + r * (BRICK_H + BRICK_GAP);
            b.w = (int)bw;
            b.h = BRICK_H;
            b.maxHp = (ch == 'b') ? 2 : (ch == 'c') ? 3 : 1;
            b.hp = b.maxHp;
            b.alive = true;
        }
    }
}

// 小球回到挡板上，进入待发球状态
static void resetBall()
{
    ball.alive = true;
    ball.pos = Vec2{ paddle.cx, (double)(PADDLE_Y - BALL_R - 2) };
    ball.vel = Vec2{ 0.0, 0.0 };
    state = ST_READY;
}

static void newGame()
{
    score = 0;
    lives = 3;
    level = 1;
    paddle.cx = WIN_W / 2.0;
    paddle.y = PADDLE_Y;
    loadLevel(level);
    resetBall();
}

// 发球：给一个朝上的初速度，水平分量带一点随机，避免每次轨迹完全一样
static void launchBall()
{
    double x = ((rand() % 200) - 100) / 100.0 * 90.0;   // -90 ~ +90 px/s
    ball.vel = Vec2{ x, -std::sqrt(LAUNCH_SPEED * LAUNCH_SPEED - x * x) };
    state = ST_PLAYING;
}

// ============================================================================
//  第 5 节：逻辑更新（固定步长积分器）
// ============================================================================

static void update(double dt)
{
    // 挡板跟随鼠标，夹在场地内
    paddle.cx = clampValue((double)gMouseX, FIELD_L + PADDLE_W / 2.0,
                           FIELD_R - PADDLE_W / 2.0);

    if (state == ST_READY) {
        ball.pos = Vec2{ paddle.cx, (double)(PADDLE_Y - BALL_R - 2) };
        return;
    }

    if (state == ST_CLEAR || state == ST_OVER || state == ST_WIN) {
        stateTimer -= dt;
        if (state == ST_CLEAR && stateTimer <= 0.0) {
            ++level;
            loadLevel(level);
            resetBall();
        }
        return;
    }

    // --- 物理积分：重力 -> 速度限幅 -> 位置推进 ---
    applyGravity(ball.vel, dt);
    clampSpeed(ball.vel);
    integratePosition(ball.pos, ball.vel, dt);

    // --- 边界碰撞 ---
    handleWalls(ball);

    // --- 挡板碰撞：仅当球在下落且与挡板重叠时触发 ---
    if (ball.vel.y > 0 &&
        ball.pos.y + BALL_R >= paddle.y &&
        ball.pos.y - BALL_R <= paddle.y + PADDLE_H &&
        fabs(ball.pos.x - paddle.cx) <= PADDLE_W / 2.0 + BALL_R) {
        bounceOffPaddle(ball, paddle);
    }

    // --- 砖块碰撞：一帧只处理一块，避免同一帧内法线互相抵消 ---
    for (int i = 0; i < brickCount; ++i) {
        Brick& b = bricks[i];
        if (!b.alive) continue;
        if (!circleRectTest(ball.pos, BALL_R, b.x, b.y, b.w, b.h)) continue;

        resolveCircleRect(ball, b.x, b.y, b.w, b.h, RESTITUTION);
        --b.hp;
        if (b.maxHp > 1) score += 5;              // 硬砖每挨一下都有分
        if (b.hp <= 0) {
            b.alive = false;
            score += (b.maxHp == 1) ? 10 : (b.maxHp == 2 ? 30 : 50);
        }
        break;
    }

    // --- 掉落判定：球心低于场地底边即失去一条命 ---
    if (ball.pos.y - BALL_R > FIELD_B) {
        --lives;
        if (lives <= 0) {
            lives = 0;
            ball.alive = false;
            state = ST_OVER;
            stateTimer = 3.0;
        } else {
            resetBall();
        }
        return;
    }

    // --- 通关判定：所有砖块都被打碎 ---
    bool any = false;
    for (int i = 0; i < brickCount; ++i) if (bricks[i].alive) { any = true; break; }
    if (!any) {
        state = (level >= LEVEL_COUNT) ? ST_WIN : ST_CLEAR;
        stateTimer = 2.0;
        score += 200 * level;                   // 过关奖励
    }
}

// ============================================================================
//  第 6 节：渲染（EasyX 绘图，与物理完全解耦）
// ============================================================================

static COLORREF brickColor(int hp, int maxHp)
{
    if (maxHp == 1) return RGB(80, 190, 255);   // 普通砖：亮蓝
    if (hp == maxHp) return RGB(255, 120, 60);  // 硬砖满血：橙
    if (hp == 2) return RGB(230, 80, 80);       // 掉一格：红
    return RGB(190, 60, 130);                   // 只剩一格：紫红
}

static void drawText(int x, int y, const TCHAR* s, COLORREF c, int size = 18)
{
    settextstyle(size, 0, L"Microsoft YaHei");
    settextcolor(c);
    outtextxy(x, y, s);
}

static void render()
{
    BeginBatchDraw();
    setbkcolor(RGB(12, 18, 36));
    cleardevice();

    // 场地边框
    setlinecolor(RGB(90, 120, 170));
    setlinestyle(PS_SOLID, 2);
    rectangle(FIELD_L, FIELD_T, FIELD_R, FIELD_B);

    // 砖块
    for (int i = 0; i < brickCount; ++i) {
        Brick& b = bricks[i];
        if (!b.alive) continue;
        COLORREF c = brickColor(b.hp, b.maxHp);
        setfillcolor(c);
        solidrectangle(b.x, b.y, b.x + b.w, b.y + b.h);
        setlinecolor(RGB(255, 255, 255));
        setlinestyle(PS_SOLID, 1);
        line(b.x, b.y, b.x + b.w, b.y);                 // 顶部高光，做出立体感
        setlinecolor(RGB(0, 0, 0));
        rectangle(b.x, b.y, b.x + b.w, b.y + b.h);

        if (b.maxHp > 1) {                              // 硬砖上标剩余耐久
            TCHAR t[4];
            _stprintf_s(t, 4, L"%d", b.hp);
            drawText(b.x + b.w / 2 - 4, b.y + 2, t, RGB(255, 255, 255), 12);
        }
    }

    // 挡板
    setfillcolor(RGB(120, 240, 180));
    setlinecolor(RGB(255, 255, 255));
    setlinestyle(PS_SOLID, 1);
    fillroundrect((int)(paddle.cx - PADDLE_W / 2), paddle.y,
                  (int)(paddle.cx + PADDLE_W / 2), paddle.y + PADDLE_H, 6, 6);

    // 小球（外圈光晕 + 实心球）
    if (ball.alive) {
        setfillcolor(RGB(90, 90, 120));
        solidcircle((int)ball.pos.x, (int)ball.pos.y, (int)BALL_R + 3);
        setfillcolor(RGB(255, 245, 200));
        solidcircle((int)ball.pos.x, (int)ball.pos.y, (int)BALL_R);
    }

    // HUD
    TCHAR buf[128];
    _stprintf_s(buf, 128, L"分数  %06d", score);   drawText(40, 22, buf, RGB(255, 225, 120), 20);
    _stprintf_s(buf, 128, L"关卡  %d / %d", level, LEVEL_COUNT);
    drawText(260, 22, buf, RGB(180, 220, 255), 20);
    _stprintf_s(buf, 128, L"生命  %d", lives);     drawText(470, 22, buf, RGB(255, 150, 150), 20);

    int left = 0;
    for (int i = 0; i < brickCount; ++i) if (bricks[i].alive) ++left;
    _stprintf_s(buf, 128, L"剩余砖块  %d", left);  drawText(600, 22, buf, RGB(200, 200, 200), 18);

    drawText(40, 600, L"鼠标移动 = 控制挡板    左键/空格 = 发球    P = 暂停    R = 重开",
             RGB(150, 170, 200), 14);

    // 状态提示
    if (state == ST_READY)
        drawText(WIN_W / 2 - 130, 470, L"点击鼠标或按空格发球", RGB(255, 240, 160), 22);
    else if (state == ST_CLEAR)
        drawText(WIN_W / 2 - 110, 330, L"过 关 !", RGB(255, 230, 100), 36);
    else if (state == ST_WIN) {
        drawText(WIN_W / 2 - 150, 300, L"全 部 通 关 !", RGB(255, 230, 100), 36);
        drawText(WIN_W / 2 - 130, 350, L"按 R 重新开始", RGB(220, 220, 220), 20);
    } else if (state == ST_OVER) {
        drawText(WIN_W / 2 - 120, 300, L"游 戏 结 束", RGB(255, 110, 110), 36);
        _stprintf_s(buf, 128, L"最终分数 %d", score);
        drawText(WIN_W / 2 - 90, 350, buf, RGB(255, 255, 255), 22);
        drawText(WIN_W / 2 - 130, 390, L"按 R 重新开始", RGB(220, 220, 220), 20);
    }

    FlushBatchDraw();
}

// ============================================================================
//  第 7 节：主循环（固定步长 + 独立渲染）
// ============================================================================

int main()
{
    initgraph(WIN_W, WIN_H);
    SetWindowText(GetHWnd(), L"物理弹球闯关 - EasyX 课程作业");
    srand((unsigned)time(NULL));

    newGame();
    bool paused = false;

    DWORD last = GetTickCount();
    double accumulator = 0.0;

    while (true) {
        // 计算真实经过时间，并限制最大补偿量，防止切走窗口回来后“物理爆炸”
        DWORD now = GetTickCount();
        double frame = (now - last) / 1000.0;
        last = now;
        if (frame > 0.05) frame = 0.05;

        // 键盘处理
        if (_kbhit()) {
            int k = _getch();
            if (k == 'r' || k == 'R') { newGame(); paused = false; }
            if (k == 'p' || k == 'P' || k == 0x1b) paused = !paused;
            if (k == ' ') {
                if (state == ST_READY) launchBall();
                else if (state == ST_OVER) newGame();
            }
        }
        // 新版 EasyX 消息接口：一次取完鼠标消息，同时更新挡板位置并响应左键发球
        ExMessage msg;
        while (peekmessage(&msg, EM_MOUSE)) {
            gMouseX = msg.x;
            if (msg.message == WM_LBUTTONDOWN && state == ST_READY) launchBall();
        }

        // 固定步长物理更新：剩余时间不足一个 FIXED_DT 就留到下一帧
        if (!paused) {
            accumulator += frame;
            while (accumulator >= FIXED_DT) {
                update(FIXED_DT);
                accumulator -= FIXED_DT;
            }
        }

        render();
        Sleep(2);                                  // 让出 CPU，约 60~120 FPS
    }

    EndBatchDraw();
    closegraph();
    return 0;
}
