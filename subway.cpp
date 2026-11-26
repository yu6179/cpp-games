// ============================================================
//  地铁跑酷 Subway Runner  ——  Windows 控制台「像素风」单文件版
//  编译：
//    g++ subway.cpp -o subway.exe -O2       (MinGW)
//    cl subway.cpp /EHsc /utf-8             (MSVC)
//    Dev-C++ / Code::Blocks：新建控制台项目粘进去，F11
//  运行：双击 subway.exe（窗口建议 60 列 x 26 行以上，字号小一点更好看）
//  操作：A/D 或 ←/→ 换道   W/↑ 跳   S/↓ 滑铲   空格 开始/重来   P 暂停   Q 退出
//
//  画面原理：
//   1) 屏幕 60x26 个字符格，用 ▀ 上下半格拆成 60x52 个「像素」，
//      一格两色（上半格=前景色，下半格=背景色），纵向分辨率翻倍；
//   2) 跑道用透视投影 u^2.2 计算每一行的宽度，越近越宽，形成 3D 纵深感；
//   3) 枕木、车道虚线随 scroll 滚动，跑起来有速度感。
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>

// ---------------- 屏幕与像素尺寸 ----------------
enum { CW = 60, CH = 26, PW = CW, PH = CH * 2 };

static const int    PX_CX       = 30;    // 跑道中心列
static const double HORIZON     = 9.0;   // 地平线像素行
static const double SPAN        = 40.0;  // 地平线到脚底的距离
static const int    PLAYER_FEET = 49;    // 玩家脚底像素行

// ---------------- 颜色（控制台 16 色） ----------------
enum {
    C_BLACK = 0, C_DBLUE = 1, C_DGREEN = 2, C_DCYAN = 3,
    C_DRED = 4, C_DMAGENTA = 5, C_BROWN = 6, C_GRAY = 7,
    C_DGRAY = 8, C_BLUE = 9, C_GREEN = 10, C_CYAN = 11,
    C_RED = 12, C_MAGENTA = 13, C_YELLOW = 14, C_WHITE = 15
};

// ---------------- 全局 ----------------
static int       pix[PH][PW];            // 像素画布（颜色索引）
static CHAR_INFO vbuf[CH][CW];
static HANDLE    hOut;

enum { E_TRAIN = 0, E_LOW = 1, E_HIGH = 2, E_COIN = 3, MAX_OBJ = 48 };

struct Obj {
    int    lane;
    int    type;
    double u;          // 0=地平线, 1=玩家位置
    bool   alive;
    bool   judged;
    int    seed;
};

static Obj   objs[MAX_OBJ];
static int   playerLane, actType, actTimer, runFrame;
static int   score, coins, best, level;
static double scroll;
static int   spawnCd, coinCd;
static bool  gameOver, paused, quitGame, started;

// ---------------- 像素绘制 ----------------
static void pset(int x, int y, int c)
{
    if (x < 0 || x >= PW || y < 0 || y >= PH) return;
    pix[y][x] = c;
}

static void frect(int x0, int y0, int x1, int y1, int c)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) pset(x, y, c);
}

static void disc(int cx, int cy, int r, int c)
{
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
            if (x * x + y * y <= r * r + 1) pset(cx + x, cy + y, c);
}

// 像素 -> 字符（上半格为前景色，下半格为背景色）
static void flushPixels()
{
    for (int cy = 0; cy < CH; ++cy)
        for (int cx = 0; cx < CW; ++cx) {
            int t = pix[cy * 2][cx];
            int b = pix[cy * 2 + 1][cx];
            if (t == b) {
                vbuf[cy][cx].Char.UnicodeChar = L' ';
                vbuf[cy][cx].Attributes       = (WORD)(t << 4);
            } else {
                vbuf[cy][cx].Char.UnicodeChar = L'\u2580';   // ▀
                vbuf[cy][cx].Attributes       = (WORD)((b << 4) | t);
            }
        }
}

static void putText(int x, int y, const char* s, WORD attr)
{
    if (y < 0 || y >= CH) return;
    for (int i = 0; s[i]; ++i) {
        int xx = x + i;
        if (xx < 0) continue;
        if (xx >= CW) break;
        vbuf[y][xx].Char.UnicodeChar = (wchar_t)(unsigned char)s[i];
        vbuf[y][xx].Attributes       = attr;
    }
}

static void present()
{
    COORD      size = { (SHORT)CW, (SHORT)CH };
    COORD      from = { 0, 0 };
    SMALL_RECT dst  = { 0, 0, (SHORT)(CW - 1), (SHORT)(CH - 1) };

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        dst.Left   = csbi.srWindow.Left;
        dst.Top    = csbi.srWindow.Top;
        dst.Right  = (SHORT)(dst.Left + CW - 1);
        dst.Bottom = (SHORT)(dst.Top  + CH - 1);
    }
    WriteConsoleOutputW(hOut, &vbuf[0][0], size, from, &dst);
}

// ---------------- 透视投影 ----------------
static inline int projY(double u)
{
    double v = pow(u, 2.2);
    return (int)(HORIZON + SPAN * v + 0.5);
}

static inline int projHalf(double u)          // 半个跑道的像素宽
{
    return (int)(2.0 + 25.0 * u * u);
}

static inline int laneCenter(int lane, int half)
{
    int lw = (half * 2) / 3;                  // 单条车道宽
    return PX_CX + (lane - 1) * lw;
}

// ---------------- 场景绘制 ----------------
static void drawGround()
{
    // 隧道背景 + 远处城市剪影
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < PW; ++x) pix[y][x] = (y < (int)HORIZON) ? C_BLACK : C_DBLUE;

    for (int x = 0; x < PW; ++x) {              // 远景楼群（静止，作为地平线参照）
        int h = 2 + (x * 37 % 7);               // 2..8 px 高
        int top = (int)HORIZON - h;
        for (int y = top; y < (int)HORIZON; ++y) {
            pix[y][x] = C_DBLUE;
            if ((x * 13 + y * 29) % 11 == 0) pix[y][x] = C_BROWN;   // 零星亮窗
        }
    }

    for (int py = (int)HORIZON; py < PH; ++py) {
        double u = pow((py - HORIZON) / SPAN, 1.0 / 2.2);
        if (u > 1.0) u = 1.0;
        int half = projHalf(u);

        double ph = u * 16.0 - scroll;
        ph -= floor(ph);
        bool tie = (ph < 0.16);                 // 细枕木

        // 路基
        frect(PX_CX - half, py, PX_CX + half, py, tie ? C_DGRAY : C_BLACK);

        // 两侧护栏（双线更像铁轨）
        pset(PX_CX - half, py, C_GRAY);
        pset(PX_CX + half, py, C_GRAY);
        if (half > 3) {
            pset(PX_CX - half + 1, py, C_DGRAY);
            pset(PX_CX + half - 1, py, C_DGRAY);
        }

        // 车道虚线
        if (ph < 0.65) {
            int lw = (half * 2) / 3;
            pset(PX_CX - lw, py, C_GRAY);
            pset(PX_CX + lw, py, C_GRAY);
        }

        // 隧道侧壁灯柱（滚动，增强速度感）
        double pp = u * 6.0 - scroll;
        pp -= floor(pp);
        if (pp < 0.10 && half > 5) {
            frect(PX_CX - half - 5, py - 3, PX_CX - half - 3, py, C_DGRAY);
            frect(PX_CX + half + 3, py - 3, PX_CX + half + 5, py, C_DGRAY);
            frect(PX_CX - half - 4, py - 3, PX_CX - half - 4, py - 2, C_BROWN);
            frect(PX_CX + half + 4, py - 3, PX_CX + half + 4, py - 2, C_BROWN);
        }
    }
}

static void drawTrain(int cx, int base, int w, int h, int seed)
{
    static const int body[5] = { C_BLUE, C_CYAN, C_RED, C_MAGENTA, C_GREEN };
    int col  = body[seed % 5];
    int half = w / 2;
    int top  = base - h;

    frect(cx - half, top, cx + half, base, col);         // 车身
    frect(cx - half, top, cx + half, top, C_WHITE);      // 车顶描边
    frect(cx - half, top, cx - half + 1, base, C_WHITE); // 左侧高光
    frect(cx + half - 1, top, cx + half, base, C_DGRAY); // 右侧暗面
    frect(cx - half, base - 1, cx + half, base, C_DGRAY);// 底部阴影

    if (w >= 7 && h >= 7) {                              // 车窗
        int winW = (w - 4) / 3;
        if (winW < 2) winW = 2;
        int wy = top + h / 3;
        for (int i = 0; i < 3; ++i) {
            int wx = cx - half + 2 + i * (winW + 1);
            frect(wx, wy, wx + winW - 1, wy + 2, C_DCYAN);
            pset(wx, wy, C_CYAN);
        }
        frect(cx - half + 2, base - 3, cx + half - 2, base - 2, C_DGRAY); // 车头格栅
        pset(cx - half + 1, base - 2, C_YELLOW);         // 车灯
        pset(cx + half - 2, base - 2, C_YELLOW);
    }
}

static void drawLowBarrier(int cx, int base, int w)
{
    int half = w / 2;
    for (int x = cx - half; x <= cx + half; ++x) {
        int c = (((x - cx + half) / 2) & 1) ? C_YELLOW : C_BLACK;
        frect(x, base - 3, x, base - 1, c);            // 黄黑警示条
    }
    frect(cx - half - 1, base - 4, cx + half + 1, base - 4, C_GRAY);
    frect(cx - half, base, cx + half, base, C_DGRAY);
}

static void drawHighBar(int cx, int base, int w, int h)
{
    int half = w / 2;
    frect(cx - half, base - h, cx - half, base, C_GRAY);      // 左立柱
    frect(cx + half, base - h, cx + half, base, C_GRAY);      // 右立柱
    frect(cx - half, base - h, cx + half, base - h + 2, C_RED); // 横杆
    frect(cx - half, base - h, cx + half, base - h, C_WHITE);
    if (h > 6) frect(cx - half + 1, base - 3, cx + half - 1, base - 2, C_DRED);
}

static void drawCoin(int cx, int cy, int r)
{
    disc(cx, cy, r, C_BROWN);
    if (r > 1) disc(cx, cy, r - 1, C_YELLOW);
    if (r > 1) pset(cx - 1, cy - 1, C_WHITE);
}

static void drawObjects()
{
    int idx[MAX_OBJ], n = 0;
    for (int i = 0; i < MAX_OBJ; ++i) if (objs[i].alive) idx[n++] = i;
    // 由远及近排序
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (objs[idx[i]].u > objs[idx[j]].u) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }

    for (int k = 0; k < n; ++k) {
        Obj& o = objs[idx[k]];
        if (o.u > 1.02) continue;
        double u = o.u < 0 ? 0 : o.u;
        int half = projHalf(u);
        int cx   = laneCenter(o.lane, half);
        int lw   = (half * 2) / 3;
        int base = projY(u);
        int w    = (int)(lw * 0.8);
        if (w < 2) w = 2;

        switch (o.type) {
            case E_TRAIN: {
                int h = (int)(4 + 17 * u * u);
                if (h < 3) h = 3;
                drawTrain(cx, base, w, h, o.seed);
                break;
            }
            case E_LOW: {
                int ww = w;
                if (ww < 3) ww = 3;
                drawLowBarrier(cx, base, ww);
                break;
            }
            case E_HIGH: {
                int h = (int)(5 + 10 * u * u);
                drawHighBar(cx, base, w, h);
                break;
            }
            case E_COIN: {
                int r  = 1 + (int)(2.4 * u * u);
                int bob = (int)(1.5 * sin((scroll + o.u) * 12.0));
                drawCoin(cx, base - 7 + bob, r);
                break;
            }
        }
    }
}

// ---------------- 玩家像素小人 ----------------
enum { P_RUN = 0, P_JUMP = 1, P_SLIDE = 2 };

static void drawPlayer(int cx, int feet, int state, int frame)
{
    const int SKIN = C_YELLOW, HAIR = C_BROWN, JACKET = C_RED,
              JEANS = C_BLUE, SHOE = C_WHITE, CAP = C_MAGENTA;

    if (state == P_SLIDE) {                      // 滑铲：横躺
        frect(cx - 6, feet - 2, cx - 3, feet, JEANS);
        frect(cx - 8, feet - 2, cx - 7, feet, SHOE);
        frect(cx - 5, feet - 6, cx + 2, feet - 3, JACKET);
        frect(cx + 3, feet - 7, cx + 6, feet - 4, SKIN);
        frect(cx + 3, feet - 8, cx + 6, feet - 8, CAP);
        pset(cx + 5, feet - 6, C_BLACK);
        return;
    }

    int top = feet - 17;

    frect(cx - 2, top, cx + 2, top + 1, CAP);          // 帽子
    frect(cx - 3, top + 1, cx + 3, top + 1, CAP);
    frect(cx - 2, top + 2, cx + 2, top + 5, SKIN);     // 脸
    pset(cx - 1, top + 3, C_BLACK);                    // 眼睛
    pset(cx + 1, top + 3, C_BLACK);
    frect(cx - 2, top + 5, cx + 2, top + 5, HAIR);

    frect(cx - 2, top + 6, cx + 2, top + 11, JACKET);  // 上身
    frect(cx - 1, top + 7, cx + 1, top + 9, C_WHITE);  // 衣服白条

    if (state == P_JUMP) {                             // 跳跃：收腿、举臂
        frect(cx - 4, top + 4, cx - 3, top + 6, SKIN);
        frect(cx + 3, top + 4, cx + 4, top + 6, SKIN);
        frect(cx - 2, top + 12, cx - 1, top + 13, JEANS);
        frect(cx + 1, top + 12, cx + 2, top + 13, JEANS);
        frect(cx - 2, top + 14, cx + 2, top + 14, SHOE);
        return;
    }

    if (frame == 0) {                                  // 跑步两帧
        frect(cx - 4, top + 6, cx - 3, top + 9, SKIN);
        frect(cx + 3, top + 8, cx + 4, top + 11, SKIN);
        frect(cx - 2, top + 12, cx - 1, top + 16, JEANS);
        frect(cx + 1, top + 12, cx + 2, top + 15, JEANS);
        frect(cx - 3, top + 17, cx - 1, top + 17, SHOE);
        frect(cx + 1, top + 16, cx + 3, top + 16, SHOE);
    } else {
        frect(cx + 3, top + 6, cx + 4, top + 9, SKIN);
        frect(cx - 4, top + 8, cx - 3, top + 11, SKIN);
        frect(cx - 2, top + 12, cx - 1, top + 15, JEANS);
        frect(cx + 1, top + 12, cx + 2, top + 16, JEANS);
        frect(cx - 3, top + 16, cx - 1, top + 16, SHOE);
        frect(cx + 1, top + 17, cx + 3, top + 17, SHOE);
    }
}

// ---------------- 逻辑 ----------------
static void addObj(int lane, int type)
{
    for (int i = 0; i < MAX_OBJ; ++i)
        if (!objs[i].alive) {
            objs[i].lane = lane; objs[i].type = type;
            objs[i].u = 0; objs[i].alive = true; objs[i].judged = false;
            objs[i].seed = rand();
            return;
        }
}

static bool laneBusy(int lane, double uMax)
{
    for (int i = 0; i < MAX_OBJ; ++i)
        if (objs[i].alive && objs[i].lane == lane && objs[i].u < uMax) return true;
    return false;
}

static void resetGame()
{
    memset(objs, 0, sizeof(objs));
    playerLane = 1; actType = 0; actTimer = 0; runFrame = 0;
    score = 0; coins = 0; level = 1; scroll = 0;
    spawnCd = 10; coinCd = 6;
    gameOver = false; paused = false; started = true;
}

static void update()
{
    score += 1;
    level = 1 + score / 300;

    double speed = 0.0062 + level * 0.0007;
    if (speed > 0.0135) speed = 0.0135;

    scroll += speed * 9.0;
    ++runFrame;

    if (actTimer > 0) { --actTimer; if (actTimer == 0) actType = 0; }

    for (int i = 0; i < MAX_OBJ; ++i) {
        if (!objs[i].alive) continue;
        objs[i].u += speed;
        if (objs[i].u > 1.05) { objs[i].alive = false; continue; }

        if (!objs[i].judged && objs[i].u >= 0.95) {
            objs[i].judged = true;
            if (objs[i].type == E_COIN) {
                if (objs[i].lane == playerLane) { ++coins; score += 10; objs[i].alive = false; }
            } else if (objs[i].lane == playerLane) {
                bool pass = (objs[i].type == E_LOW  && actType == 1) ||
                            (objs[i].type == E_HIGH && actType == 2);
                if (!pass) { gameOver = true; if (score > best) best = score; Beep(160, 160); return; }
            }
        }
    }

    // 刷障碍（一次只堵一条道，永远有活路）
    int cd = 26 - level * 2;
    if (cd < 12) cd = 12;
    if (--spawnCd <= 0) {
        int lane = rand() % 3;
        if (!laneBusy(lane, 0.12)) {
            int r = rand() % 100;
            addObj(lane, (r < 45) ? E_TRAIN : (r < 75 ? E_LOW : E_HIGH));
        }
        spawnCd = cd + rand() % 10;
    }

    // 刷金币
    if (--coinCd <= 0) {
        int lane = rand() % 3;
        if (!laneBusy(lane, 0.10)) addObj(lane, E_COIN);
        coinCd = 22 + rand() % 18;
    }
}

// ---------------- 一帧 ----------------
static void drawFrame()
{
    drawGround();
    drawObjects();

    int half = projHalf(1.0);
    int cx   = laneCenter(playerLane, half);
    if (started) {
        if (actType == 1)      drawPlayer(cx, PLAYER_FEET - 5, P_JUMP, 0);
        else if (actType == 2) drawPlayer(cx, PLAYER_FEET,     P_SLIDE, 0);
        else                   drawPlayer(cx, PLAYER_FEET,     P_RUN, (runFrame / 4) & 1);

        // 脚下阴影
        frect(cx - 4, PLAYER_FEET + 1, cx + 4, PLAYER_FEET + 1, C_DGRAY);
    }

    flushPixels();

    // ---- HUD ----
    char buf[64];
    putText(1, 0, "SCORE", C_CYAN | 0);
    sprintf(buf, "%d", score);  putText(7, 0, buf, C_WHITE);
    putText(17, 0, "BEST", C_CYAN);
    sprintf(buf, "%d", best);   putText(22, 0, buf, C_WHITE);
    putText(32, 0, "COIN", C_CYAN);
    sprintf(buf, "%d", coins);  putText(37, 0, buf, C_YELLOW);
    putText(46, 0, "SPEED", C_CYAN);
    sprintf(buf, "%d", level);  putText(52, 0, buf, C_WHITE);

    putText(1, 25, "A/D MOVE  W JUMP  S SLIDE  P PAUSE  Q QUIT", C_GRAY);

    // ---- 覆盖层 ----
    if (!started) {
        frect(6, 20, 54, 44, C_BLACK);
        flushPixels();
        putText(20, 11, "S U B W A Y   R U N", C_WHITE);
        putText(17, 13, "A/D MOVE   W JUMP   S SLIDE", C_GRAY);
        putText(22, 15, "PRESS SPACE TO START", C_YELLOW);
    }
    if (paused && started && !gameOver) {
        putText(25, 12, "P A U S E D", C_WHITE);
    }
    if (gameOver) {
        frect(10, 22, 50, 40, C_BLACK);
        flushPixels();
        putText(23, 12, "G A M E  O V E R", C_RED);
        sprintf(buf, "SCORE %d   COIN %d", score, coins);
        putText(22, 14, buf, C_WHITE);
        putText(20, 16, "SPACE RETRY     Q QUIT", C_GRAY);
    }

    present();
}

// ---------------- 键盘 ----------------
static int readKey()
{
    if (!_kbhit()) return -1;
    int c = _getch();
    if (c == 0 || c == 224) {
        if (!_kbhit()) return -1;
        return 1000 + _getch();
    }
    return c;
}

// ---------------- 主流程 ----------------
int main()
{
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(hOut, &ci)) { ci.bVisible = FALSE; SetConsoleCursorInfo(hOut, &ci); }
    SetConsoleTitleW(L"Subway Runner");

    srand((unsigned)time(NULL));
    memset(objs, 0, sizeof(objs));
    best = 0; started = false; gameOver = false; paused = false;
    playerLane = 1; actType = 0; actTimer = 0; score = 0; coins = 0;
    level = 1; scroll = 0;

    const int TICK = 33;
    DWORD nextTick = GetTickCount();

    while (!quitGame) {
        int k;
        while ((k = readKey()) != -1) {
            if (k == 'q' || k == 'Q') { quitGame = true; break; }

            if (!started || gameOver) {
                if (k == ' ') { resetGame(); nextTick = GetTickCount() + TICK; }
                continue;
            }
            if (k == 'p' || k == 'P') { paused = !paused; continue; }
            if (paused) continue;

            switch (k) {
                case 1000 + 75: case 'a': case 'A': if (playerLane > 0) --playerLane; break;
                case 1000 + 77: case 'd': case 'D': if (playerLane < 2) ++playerLane; break;
                case 1000 + 72: case 'w': case 'W':
                    if (actType == 0) { actType = 1; actTimer = 12; }
                    break;
                case 1000 + 80: case 's': case 'S':
                    if (actType == 0) { actType = 2; actTimer = 12; }
                    break;
                default: break;
            }
        }
        if (quitGame) break;

        if (started && !gameOver && !paused) {
            DWORD now = GetTickCount();
            if (now >= nextTick) {
                update();
                nextTick = now + TICK;
            }
        }

        drawFrame();
        Sleep(10);
    }

    if (GetConsoleCursorInfo(hOut, &ci)) { ci.bVisible = TRUE; SetConsoleCursorInfo(hOut, &ci); }
    return 0;
}
