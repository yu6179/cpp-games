// ============================================================
//  坦克大战 Tank Battle  ——  Windows 控制台单文件版
//  编译：
//    g++ tank.cpp -o tank.exe -O2           (MinGW)
//    cl tank.cpp /EHsc /utf-8               (MSVC)
//    Dev-C++ / Code::Blocks：新建控制台项目粘进来，F11
//  运行：tank.exe                     单人
//        tank.exe host                局域网主机（先运行，屏幕会显示本机 IP）
//        tank.exe join 192.168.1.5    局域网加入（填主机的 IP）
//  操作：方向键/WASD 移动   空格/J 开炮   P 暂停   R 重开   Q 退出
//  目标：守住底部老鹰基地，消灭每关 20 辆敌方坦克。
//  道具：★升级  ¤手雷  O头盔  #铲子  +生命  T时钟
//  联机：TCP 7777 端口，主机权威。主机跑全部逻辑，客户端只发按键、收快照。
//        MinGW 编译要加 -lws2_32：g++ tank.cpp -o tank.exe -O2 -lws2_32
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

// ---------------- 屏幕 ----------------
enum { CW = 80, CH = 30, PW = CW, PH = CH * 2 };
enum { C_BLACK=0, C_DBLUE=1, C_DGREEN=2, C_DCYAN=3, C_DRED=4, C_DMAGENTA=5,
       C_BROWN=6, C_GRAY=7, C_DGRAY=8, C_BLUE=9, C_GREEN=10, C_CYAN=11,
       C_RED=12, C_MAGENTA=13, C_YELLOW=14, C_WHITE=15 };

// ---------------- 战场 ----------------
enum { TS = 4, MW = 13, MH = 13, FX = 2, FY = 4 };     // 每格 4 像素，13x13 格
enum { FIELD_W = MW * TS, FIELD_H = MH * TS };

static int       pix[PH][PW];
static CHAR_INFO vbuf[CH][CW];
static HANDLE    hOut;
static HANDLE    hIn;

// 键盘状态：用 ReadConsoleInput 自己维护，避免依赖 user32 的 GetAsyncKeyState
static bool keyDown[256];
static bool keyHit[256];

// ---------------- 关卡地图 ----------------
// '.'' 空  '#'砖墙  '@'钢墙  '~'河流  '*'草地  'B'基地
static const char* MAPS[3][MH] = {
    {
        ".............",
        ".............",
        "..#.#...#.#..",
        "..#.#...#.#..",
        "....#...#....",
        "##..#...#..##",
        "....#####....",
        "..#.......#..",
        "..#.##.##.#..",
        "..#.......#..",
        ".....###.....",
        ".....###.....",
        ".....#B#....."
    },
    {
        ".............",
        "..@.......@..",
        "..@.#####.@..",
        "....#...#....",
        ".##.#.#.#.##.",
        ".##...#...##.",
        "...~~...~~...",
        ".##...#...##.",
        ".##.#.#.#.##.",
        "....#.#.#....",
        "..***#.#***..",
        ".....###.....",
        ".....#B#....."
    },
    {
        ".............",
        "..#.......#..",
        "..#.@@@@@.#..",
        "..#.......#..",
        "..#####.###..",
        "..#...~...#..",
        "..#.#~~~#.#..",
        "..#.#~~~#.#..",
        "..#...~...#..",
        ".###.....##..",
        "..*..###..*..",
        ".....###.....",
        ".....#B#....."
    }
};

static char map[MH][MW + 1];

// ---------------- 实体 ----------------
enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 };
enum { MAX_ENEMY = 8, MAX_BULLET = 16, MAX_BOOM = 16, MAX_ITEM = 4 };
enum { IT_STAR = 0, IT_GRENADE, IT_HELMET, IT_SHOVEL, IT_LIFE, IT_CLOCK };
enum { ST_TITLE = 0, ST_PLAY, ST_PAUSE, ST_CLEAR, ST_OVER };

struct Tank {
    bool  alive;
    float x, y;
    int   dir;
    int   kind;        // 0=玩家 1..4=敌方
    int   hp;
    int   cd;
    bool  bonus;       // 闪烁的奖励坦克
    int   spawn;       // 出生动画剩余帧
    int   inv;         // 无敌帧
    int   mvTimer;     // 换向计时
    int   fireTimer;
};

struct Bullet { bool alive; float x, y; int dir; int owner; int power; };   // owner: 0=敌方 1=P1 2=P2
struct Boom   { bool alive; float x, y; int frame; };
struct Item   { bool alive; float x, y; int type; };

static Tank   player;
static Tank   player2;          // 联机时的 2P
static Tank   en[MAX_ENEMY];
static Bullet bul[MAX_BULLET];
static Boom   booms[MAX_BOOM];
static Item   items[MAX_ITEM];

static int  score, hiScore, lives, level, starLv;
static int  enemyLeft;          // 本关剩余待出场
static int  state;
static int  freezeTimer, helmetTimer, shovelTimer;
static int  spawnTimer, nextSpawnIdx;
static bool baseAlive;
static long tick;

static bool p2On = false;       // 2P 是否在场（联机且未出局）
static int  lives2 = 3;
static int  c1dir = -1, c2dir = -1;     // 本帧的方向输入，-1 表示没按
static bool c1fire = false, c2fire = false;

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
                vbuf[cy][cx].Char.UnicodeChar = L'\u2580';   // ▀ 半格像素
                vbuf[cy][cx].Attributes       = (WORD)((b << 4) | t);
            }
        }
}

static void drawText(int x, int y, const char* s, WORD attr)
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
        dst.Bottom = (SHORT)(dst.Top + CH - 1);
    }
    WriteConsoleOutputW(hOut, &vbuf[0][0], size, from, &dst);
}

// ---------------- 地图工具 ----------------
static bool cellSolid(int tx, int ty, bool forBullet)
{
    if (tx < 0 || tx >= MW || ty < 0 || ty >= MH) return true;
    char c = map[ty][tx];
    if (c == '#' || c == '@') return true;
    if (c == 'B') return true;
    if (c == '~' && !forBullet) return true;
    return false;
}

static bool rectHitsMap(float x, float y, float w, float h, bool forBullet)
{
    int x0 = (int)floor(x / TS), x1 = (int)floor((x + w - 0.01f) / TS);
    int y0 = (int)floor(y / TS), y1 = (int)floor((y + h - 0.01f) / TS);
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx)
            if (cellSolid(tx, ty, forBullet)) return true;
    return false;
}

static bool overlap(float ax, float ay, float bx, float by)
{
    return ax < bx + 8 && bx < ax + 8 && ay < by + 8 && by < ay + 8;
}

static bool bulletHitsTank(const Bullet& b, const Tank& t)
{
    return b.x + 2 > t.x && b.x < t.x + 8 && b.y + 2 > t.y && b.y < t.y + 8;
}

// ---------------- 联机：TCP 主机权威 ----------------
//  主机：跑全部逻辑 -> 每帧把整个世界打包成快照发给客户端
//  客户端：只把按键发过去，收到快照就覆盖本地状态再渲染
//  好处：不可能出现"两边算出不同世界"，丢包/延迟只影响画面新鲜度
enum { NET_NONE = 0, NET_HOST, NET_CLIENT };
enum { NET_PORT = 7777 };
enum { PKT_INPUT = 1, PKT_SNAP = 2 };

struct PktInput {
    int type;
    int dir;        // -1 无
    int fire;
    int pad;
};

struct PktSnap {
    int    type;
    int    seq;
    Tank   p1, p2;
    Tank   en[MAX_ENEMY];
    Bullet bul[MAX_BULLET];
    Boom   booms[MAX_BOOM];
    Item   items[MAX_ITEM];
    char   map[MH][MW + 1];
    int    score, lives, lives2, level, starLv, enemyLeft, st;
    int    baseAliveI, freezeTimer, helmetTimer, shovelTimer, tickI, p2OnI;
};

static int    netMode = NET_NONE;
static SOCKET netSock = INVALID_SOCKET;
static SOCKET listenSock = INVALID_SOCKET;
static bool   netOn = false;            // 已连上
static bool   netConnecting = false;
static DWORD  netRetry = 0;
static char   rxBuf[8192];
static int    rxLen = 0;
static int    netSeq = 0;
static char   netIp[64];

static void netSetNonBlock(SOCKET s)
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

static void netNoDelay(SOCKET s)        // 关掉 Nagle，否则每次按键要等 40ms
{
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

static const char* netLocalIp()
{
    if (netIp[0]) return netIp;
    strcpy(netIp, "127.0.0.1");
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
        hostent* he = gethostbyname(host);
        if (he && he->h_addr_list[0]) {
            in_addr a;
            memcpy(&a, he->h_addr_list[0], 4);
            strncpy(netIp, inet_ntoa(a), sizeof(netIp) - 1);
        }
    }
    return netIp;
}

static bool netStartup()
{
    WSADATA wd;
    return WSAStartup(MAKEWORD(2, 2), &wd) == 0;
}

static bool netListen(int port)
{
    listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((u_short)port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) return false;
    if (listen(listenSock, 1) == SOCKET_ERROR) return false;
    netSetNonBlock(listenSock);
    return true;
}

static bool netAcceptPoll()
{
    if (netOn || listenSock == INVALID_SOCKET) return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(listenSock, &rd);
    timeval tv = { 0, 0 };
    if (select(0, &rd, NULL, NULL, &tv) <= 0) return false;
    SOCKET s = accept(listenSock, NULL, NULL);
    if (s == INVALID_SOCKET) return false;
    netSock = s;
    netSetNonBlock(netSock);
    netNoDelay(netSock);
    rxLen = 0;
    netOn = true;
    return true;
}

static bool netConnectStart(const char* ip)
{
    netSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (netSock == INVALID_SOCKET) return false;

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u_short)NET_PORT);
    sa.sin_addr.s_addr = inet_addr(ip);
    if (sa.sin_addr.s_addr == INADDR_NONE) {          // 不是点分 IP，试主机名
        hostent* he = gethostbyname(ip);
        if (!he || !he->h_addr_list[0]) return false;
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    }

    netSetNonBlock(netSock);
    int r = connect(netSock, (sockaddr*)&sa, sizeof(sa));
    if (r == 0) {
        netNoDelay(netSock);
        netOn = true;
        netConnecting = false;
        rxLen = 0;
        return true;
    }
    netConnecting = (WSAGetLastError() == WSAEWOULDBLOCK);
    if (!netConnecting) { closesocket(netSock); netSock = INVALID_SOCKET; }
    return netConnecting;
}

static int netConnectPoll()      // 0=还在连 1=成功 -1=失败
{
    if (netOn) return 1;
    if (!netConnecting) return -1;
    fd_set wr, ex;
    FD_ZERO(&wr); FD_SET(netSock, &wr);
    FD_ZERO(&ex); FD_SET(netSock, &ex);
    timeval tv = { 0, 0 };
    if (select(0, NULL, &wr, &ex, &tv) <= 0) return 0;
    if (FD_ISSET(netSock, &ex)) { netConnecting = false; closesocket(netSock); netSock = INVALID_SOCKET; return -1; }
    if (FD_ISSET(netSock, &wr)) {
        netConnecting = false;
        netNoDelay(netSock);
        netOn = true;
        rxLen = 0;
        return 1;
    }
    return 0;
}

static void netDrop()
{
    if (netSock != INVALID_SOCKET) { closesocket(netSock); netSock = INVALID_SOCKET; }
    netOn = false;
    netConnecting = false;
    rxLen = 0;
}

// 把内核缓冲里的数据搬到 rxBuf；返回 -1 表示对端已断开
static int netPump()
{
    if (!netOn) return 0;
    if (rxLen >= (int)sizeof(rxBuf)) rxLen = 0;         // 防御：异常数据只丢弃，不越界
    int n = recv(netSock, rxBuf + rxLen, (int)sizeof(rxBuf) - rxLen, 0);
    if (n > 0) { rxLen += n; return n; }
    if (n == 0) { netDrop(); return -1; }
    if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
    netDrop();
    return -1;
}

static bool netTake(void* dst, int sz)
{
    if (rxLen < sz) return false;
    memcpy(dst, rxBuf, sz);
    memmove(rxBuf, rxBuf + sz, rxLen - sz);
    rxLen -= sz;
    return true;
}

static bool netSendAll(const void* p, int n)
{
    if (!netOn) return false;
    const char* c = (const char*)p;
    int sent = 0, guard = 0;
    while (sent < n && guard++ < 500) {
        int r = send(netSock, c + sent, n - sent, 0);
        if (r > 0) { sent += r; continue; }
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(netSock, &wr);
            timeval tv = { 0, 2000 };                    // 最多等 2ms
            select(0, NULL, &wr, NULL, &tv);
            continue;
        }
        netDrop();
        return false;
    }
    return sent == n;
}

static void packSnap(PktSnap& s)
{
    memset(&s, 0, sizeof(s));
    s.type = PKT_SNAP;
    s.seq  = ++netSeq;
    s.p1 = player;
    s.p2 = player2;
    memcpy(s.en,    en,    sizeof(en));
    memcpy(s.bul,   bul,   sizeof(bul));
    memcpy(s.booms, booms, sizeof(booms));
    memcpy(s.items, items, sizeof(items));
    memcpy(s.map,   map,   sizeof(map));
    s.score = score; s.lives = lives; s.lives2 = lives2; s.level = level;
    s.starLv = starLv; s.enemyLeft = enemyLeft; s.st = state;
    s.baseAliveI   = baseAlive ? 1 : 0;
    s.freezeTimer  = freezeTimer;
    s.helmetTimer  = helmetTimer;
    s.shovelTimer  = shovelTimer;
    s.tickI        = (int)tick;
    s.p2OnI        = p2On ? 1 : 0;
}

static void applySnap(const PktSnap& s)
{
    player  = s.p1;
    player2 = s.p2;
    memcpy(en,    s.en,    sizeof(en));
    memcpy(bul,   s.bul,   sizeof(bul));
    memcpy(booms, s.booms, sizeof(booms));
    memcpy(items, s.items, sizeof(items));
    memcpy(map,   s.map,   sizeof(map));
    score = s.score; lives = s.lives; lives2 = s.lives2; level = s.level;
    starLv = s.starLv; enemyLeft = s.enemyLeft; state = s.st;
    baseAlive    = s.baseAliveI != 0;
    freezeTimer  = s.freezeTimer;
    helmetTimer  = s.helmetTimer;
    shovelTimer  = s.shovelTimer;
    tick         = s.tickI;
    p2On         = s.p2OnI != 0;
}

// ---------------- 爆炸 / 子弹 ----------------
static void addBoom(float x, float y)
{
    for (int i = 0; i < MAX_BOOM; ++i)
        if (!booms[i].alive) { booms[i].alive = true; booms[i].x = x; booms[i].y = y; booms[i].frame = 0; return; }
}

static void fire(Tank& t, int owner, int power)
{
    int total = 0;
    for (int i = 0; i < MAX_BULLET; ++i) if (bul[i].alive && bul[i].owner == owner) ++total;
    int limit = (owner != 0) ? (starLv >= 2 ? 2 : 1) : 1;    // 玩家子弹上限按人算
    if (total >= limit) return;

    for (int i = 0; i < MAX_BULLET; ++i) {
        if (bul[i].alive) continue;
        bul[i].alive = true;
        bul[i].owner = owner;
        bul[i].power = power;
        bul[i].dir = t.dir;
        float cx = t.x + 4, cy = t.y + 4;
        if (t.dir == DIR_UP)    { bul[i].x = cx - 1; bul[i].y = cy - 6; }
        if (t.dir == DIR_DOWN)  { bul[i].x = cx - 1; bul[i].y = cy + 2; }
        if (t.dir == DIR_LEFT)  { bul[i].x = cx - 6; bul[i].y = cy - 1; }
        if (t.dir == DIR_RIGHT) { bul[i].x = cx + 2; bul[i].y = cy - 1; }
        return;
    }
}

static void hitBase()
{
    baseAlive = false;
    addBoom(6 * TS, 12 * TS);
    addBoom(6 * TS + 2, 12 * TS + 2);
    state = ST_OVER;
}

// ---------------- 图像：地形 / 坦克 / 道具 ----------------
static void drawTile(int tx, int ty)
{
    int px = FX + tx * TS, py = FY + ty * TS;
    char c = map[ty][tx];
    switch (c) {
        case '#':
            frect(px, py, px + 3, py + 3, C_BROWN);
            frect(px, py, px + 3, py, C_DGRAY);
            frect(px, py + 2, px + 3, py + 2, C_DGRAY);
            pset(px + 1, py + 1, C_DGRAY);
            pset(px + 3, py + 3, C_DGRAY);
            break;
        case '@':
            frect(px, py, px + 3, py + 3, C_GRAY);
            frect(px + 1, py + 1, px + 2, py + 2, C_WHITE);
            break;
        case '~': {
            frect(px, py, px + 3, py + 3, C_BLUE);
            int ph = (int)((tick / 6 + tx * 3) % 4);
            pset(px + ph, py + 1, C_CYAN);
            pset(px + (ph + 2) % 4, py + 3, C_CYAN);
            break;
        }
        case '*':
            frect(px, py, px + 3, py + 3, C_DGREEN);   // 草地地面，草叶在上层叠加
            break;
        case 'B':
            if (baseAlive) {
                frect(px, py, px + 3, py + 3, C_DGRAY);
                frect(px + 1, py + 1, px + 2, py + 3, C_YELLOW);
                pset(px + 1, py, C_YELLOW); pset(px + 2, py, C_YELLOW);
                pset(px, py + 1, C_BROWN);  pset(px + 3, py + 1, C_BROWN);
            } else {
                frect(px, py, px + 3, py + 3, C_DGRAY);
                pset(px, py + 2, C_BLACK); pset(px + 2, py + 1, C_BLACK);
                pset(px + 3, py + 3, C_BLACK);
            }
            break;
        default:
            frect(px, py, px + 3, py + 3, C_BLACK);
            break;
    }
}

static void drawGrassOverlay(int tx, int ty)
{
    int px = FX + tx * TS, py = FY + ty * TS;
    frect(px, py, px + 3, py + 3, C_GREEN);
    pset(px, py + 1, C_DGREEN); pset(px + 2, py, C_DGREEN); pset(px + 3, py + 2, C_DGREEN);
}

static void drawTank(const Tank& t)
{
    if (t.spawn > 0) {                                  // 出生动画：闪烁四角星
        int px = FX + (int)t.x, py = FY + (int)t.y;
        if ((t.spawn / 3) % 2 == 0) {
            pset(px + 3, py + 1, C_WHITE); pset(px + 4, py + 1, C_WHITE);
            pset(px + 1, py + 3, C_WHITE); pset(px + 1, py + 4, C_WHITE);
            pset(px + 6, py + 3, C_WHITE); pset(px + 6, py + 4, C_WHITE);
            pset(px + 3, py + 6, C_WHITE); pset(px + 4, py + 6, C_WHITE);
        }
        return;
    }
    if (t.inv > 0 && (t.inv / 2) % 2 == 0) return;       // 无敌闪烁

    int body = C_GREEN, trim = C_DGREEN;
    if (&t == &player2) { body = C_WHITE; trim = C_YELLOW; }   // 2P：白车黄履带，一眼分得清
    if (t.kind == 1) { body = C_GRAY;    trim = C_DGRAY; }
    if (t.kind == 2) { body = C_CYAN;    trim = C_DCYAN; }
    if (t.kind == 3) { body = (t.hp > 1) ? C_YELLOW : C_RED; trim = C_BROWN; }
    if (t.kind == 4) { body = C_MAGENTA; trim = C_DMAGENTA; }
    if (t.kind > 0 && t.bonus && ((tick / 5) % 2 == 0)) { body = C_WHITE; trim = C_GRAY; }

    int px = FX + (int)t.x, py = FY + (int)t.y;
    frect(px, py, px + 7, py + 7, trim);                 // 履带底色
    frect(px + 1, py + 1, px + 6, py + 6, body);         // 车体

    if (t.dir == DIR_UP)    frect(px + 3, py - 1, px + 4, py + 1, C_WHITE);
    if (t.dir == DIR_DOWN)  frect(px + 3, py + 6, px + 4, py + 8, C_WHITE);
    if (t.dir == DIR_LEFT)  frect(px - 1, py + 3, px + 1, py + 4, C_WHITE);
    if (t.dir == DIR_RIGHT) frect(px + 6, py + 3, px + 8, py + 4, C_WHITE);

    for (int i = 0; i < 8; i += 2) {                     // 履带纹
        pset(px, py + i, C_BLACK);
        pset(px + 7, py + i, C_BLACK);
    }
}

static void drawBoom(const Boom& b)
{
    int px = FX + (int)b.x + 4, py = FY + (int)b.y + 4;
    int r = 1 + b.frame;
    int c1 = (b.frame < 2) ? C_WHITE : (b.frame < 4 ? C_YELLOW : C_RED);
    int c2 = (b.frame < 2) ? C_YELLOW : C_RED;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            int d2 = x * x + y * y;
            if (d2 <= r * r)      pset(px + x, py + y, c1);
            else if (d2 <= r * r + 2 * r + 1) pset(px + x, py + y, c2);
        }
}

static void drawItem(const Item& it)
{
    int px = FX + (int)it.x, py = FY + (int)it.y;
    frect(px, py, px + 5, py + 5, C_WHITE);
    frect(px + 1, py + 1, px + 4, py + 4, (it.type == IT_STAR) ? C_YELLOW : C_DCYAN);
    switch (it.type) {
        case IT_STAR:
            pset(px + 2, py + 1, C_WHITE); pset(px + 3, py + 1, C_WHITE);
            frect(px + 1, py + 2, px + 4, py + 3, C_YELLOW);
            break;
        case IT_GRENADE:
            frect(px + 2, py + 1, px + 3, py + 4, C_BLACK);
            pset(px + 4, py + 1, C_RED);
            break;
        case IT_HELMET:
            frect(px + 1, py + 2, px + 4, py + 3, C_CYAN);
            frect(px + 2, py + 1, px + 3, py + 1, C_CYAN);
            break;
        case IT_SHOVEL:
            frect(px + 2, py + 1, px + 3, py + 3, C_BROWN);
            frect(px + 1, py + 3, px + 4, py + 4, C_GRAY);
            break;
        case IT_LIFE:
            pset(px + 2, py + 1, C_WHITE); pset(px + 3, py + 2, C_WHITE);
            pset(px + 2, py + 2, C_WHITE); pset(px + 2, py + 3, C_WHITE);
            break;
        case IT_CLOCK:
            frect(px + 2, py + 1, px + 2, py + 3, C_WHITE);
            frect(px + 2, py + 3, px + 3, py + 3, C_WHITE);
            break;
    }
}

// ---------------- 逻辑 ----------------
static void loadLevel(int lv)
{
    const char** src = MAPS[(lv - 1) % 3];
    for (int y = 0; y < MH; ++y) {
        for (int x = 0; x < MW; ++x) map[y][x] = src[y][x];
        map[y][MW] = 0;
    }
    enemyLeft = 20;
    spawnTimer = 0;
    nextSpawnIdx = 0;
    freezeTimer = helmetTimer = shovelTimer = 0;
    baseAlive = true;

    for (int i = 0; i < MAX_ENEMY; ++i) en[i].alive = false;
    for (int i = 0; i < MAX_BULLET; ++i) bul[i].alive = false;
    for (int i = 0; i < MAX_BOOM; ++i) booms[i].alive = false;
    for (int i = 0; i < MAX_ITEM; ++i) items[i].alive = false;

    player.alive = true; player.x = 3 * TS; player.y = 11 * TS;
    player.dir = DIR_UP; player.kind = 0; player.hp = 1; player.cd = 0;
    player.spawn = 0; player.inv = 60; player.bonus = false;

    if (p2On) {                                     // 2P 从右侧出生
        player2.alive = true; player2.x = 9 * TS; player2.y = 11 * TS;
        player2.dir = DIR_UP; player2.kind = 0; player2.hp = 1; player2.cd = 0;
        player2.spawn = 0; player2.inv = 60; player2.bonus = false;
        lives2 = 3;
    } else {
        player2.alive = false;
    }
}

static void resetGame()
{
    score = 0; lives = 3; level = 1; starLv = 1;
    state = ST_PLAY;
    loadLevel(level);
}

static bool tryMove(Tank& t, float dx, float dy)
{
    float nx = t.x + dx, ny = t.y + dy;
    if (nx < 0 || ny < 0 || nx + 8 > FIELD_W || ny + 8 > FIELD_H) return false;
    if (rectHitsMap(nx, ny, 8, 8, false)) return false;
    if (&t != &player && player.alive && player.spawn == 0 && overlap(nx, ny, player.x, player.y)) return false;
    if (&t != &player2 && p2On && player2.alive && player2.spawn == 0 &&
        overlap(nx, ny, player2.x, player2.y)) return false;
    for (int i = 0; i < MAX_ENEMY; ++i) {
        Tank& o = en[i];
        if (!o.alive || &o == &t || o.spawn > 0) continue;
        if (overlap(nx, ny, o.x, o.y)) return false;
    }
    t.x = nx; t.y = ny;
    return true;
}

static void setDir(Tank& t, int d)
{
    if (t.dir == d) return;
    float ox = t.x, oy = t.y;
    t.dir = d;
    if (d == DIR_UP || d == DIR_DOWN)                        // 转成上下时对齐 x
        t.x = floor(t.x / TS + 0.5f) * TS;
    else
        t.y = floor(t.y / TS + 0.5f) * TS;
    if (rectHitsMap(t.x, t.y, 8, 8, false)) { t.x = ox; t.y = oy; }
}

static void damageEnemy(Tank& e)
{
    if (--e.hp > 0) return;
    e.alive = false;
    addBoom(e.x, e.y);
    score += (e.kind == 1 ? 100 : e.kind == 2 ? 200 : e.kind == 3 ? 400 : 300);
    if (e.bonus) {
        for (int i = 0; i < MAX_ITEM; ++i)
            if (!items[i].alive) {
                items[i].alive = true;
                items[i].type = rand() % 6;
                items[i].x = (float)((rand() % 9 + 2) * TS);
                items[i].y = (float)((rand() % 9 + 2) * TS);
                break;
            }
    }
}

static void updateBullets()
{
    for (int i = 0; i < MAX_BULLET; ++i) {
        Bullet& b = bul[i];
        if (!b.alive) continue;
        float dx = 0, dy = 0;
        if (b.dir == DIR_UP) dy = -1;
        if (b.dir == DIR_DOWN) dy = 1;
        if (b.dir == DIR_LEFT) dx = -1;
        if (b.dir == DIR_RIGHT) dx = 1;

        for (int s = 0; s < 2 && b.alive; ++s) {
            b.x += dx; b.y += dy;

            if (b.x < 0 || b.y < 0 || b.x > FIELD_W - 2 || b.y > FIELD_H - 2) { b.alive = false; break; }

            // 打地形
            int tx = (int)floor((b.x + 1) / TS), ty = (int)floor((b.y + 1) / TS);
            if (tx >= 0 && tx < MW && ty >= 0 && ty < MH) {
                char c = map[ty][tx];
                if (c == 'B') { b.alive = false; hitBase(); break; }
                if (c == '#') {
                    map[ty][tx] = '.';
                    addBoom((float)(tx * TS), (float)(ty * TS));
                    b.alive = false; break;
                }
                if (c == '@') {
                    if (b.owner != 0 && b.power >= 2) map[ty][tx] = '.';
                    addBoom((float)(tx * TS), (float)(ty * TS));
                    b.alive = false; break;
                }
            }

            // 打子弹
            for (int j = 0; j < MAX_BULLET; ++j) {
                if (j == i || !bul[j].alive) continue;
                if ((bul[j].owner == 0) == (b.owner == 0)) continue;     // 同阵营不互撞
                if (fabs(b.x - bul[j].x) < 2.5f && fabs(b.y - bul[j].y) < 2.5f) {
                    b.alive = false; bul[j].alive = false; break;
                }
            }
            if (!b.alive) break;

            // 打坦克
            if (b.owner != 0) {
                for (int k = 0; k < MAX_ENEMY; ++k) {
                    Tank& e = en[k];
                    if (!e.alive || e.spawn > 0) continue;
                    if (bulletHitsTank(b, e)) {
                        b.alive = false;
                        damageEnemy(e);
                        break;
                    }
                }
            } else {
                if (player.alive && player.spawn == 0 && player.inv == 0 && bulletHitsTank(b, player)) {
                    b.alive = false;
                    addBoom(player.x, player.y);
                    if (--lives < 0) { state = ST_OVER; return; }
                    player.x = 3 * TS; player.y = 11 * TS;
                    player.dir = DIR_UP; player.inv = 90; starLv = 1;
                }
                else if (p2On && player2.alive && player2.spawn == 0 && player2.inv == 0 &&
                         bulletHitsTank(b, player2)) {
                    b.alive = false;
                    addBoom(player2.x, player2.y);
                    if (--lives2 < 0) {                         // 2P 命用完：出局，1P 继续
                        player2.alive = false;
                        p2On = false;
                    } else {
                        player2.x = 9 * TS; player2.y = 11 * TS;
                        player2.dir = DIR_UP; player2.inv = 90;
                    }
                }
            }
        }
    }
}

static void controlTank(Tank& t, int d, bool fireKey, int owner)
{
    if (t.inv > 0) --t.inv;
    if (t.cd > 0) --t.cd;

    if (d >= 0) {
        setDir(t, d);
        float sp = 1.0f;
        if (d == DIR_UP)    tryMove(t, 0, -sp);
        if (d == DIR_DOWN)  tryMove(t, 0, sp);
        if (d == DIR_LEFT)  tryMove(t, -sp, 0);
        if (d == DIR_RIGHT) tryMove(t, sp, 0);
    }
    if (fireKey && t.cd == 0) {
        fire(t, owner, starLv >= 3 ? 2 : 1);
        t.cd = 8;
    }
}

static void updatePlayer()
{
    if (!player.alive) return;
    controlTank(player, c1dir, c1fire, 1);
}

static void updatePlayer2()
{
    if (!p2On || !player2.alive) return;
    controlTank(player2, c2dir, c2fire, 2);
}

// 本地键盘 -> 控制意图。主机算 P1；客户端把结果发给主机当 P2
static void readLocalCtrl(int& d, bool& f)
{
    d = -1;
    if (keyDown[VK_UP]    || keyDown['W']) d = DIR_UP;
    else if (keyDown[VK_DOWN]  || keyDown['S']) d = DIR_DOWN;
    else if (keyDown[VK_LEFT]  || keyDown['A']) d = DIR_LEFT;
    else if (keyDown[VK_RIGHT] || keyDown['D']) d = DIR_RIGHT;
    f = (keyDown[VK_SPACE] || keyDown['J']);
}

static int chooseDir(Tank& t)
{
    if (rand() % 100 < 40) {                       // 40% 概率朝基地方向
        float bx = 6 * TS - t.x, by = 12 * TS - t.y;
        if (fabs(bx) > fabs(by)) return bx > 0 ? DIR_RIGHT : DIR_LEFT;
        return by > 0 ? DIR_DOWN : DIR_UP;
    }
    return rand() % 4;
}

static void updateEnemies()
{
    for (int i = 0; i < MAX_ENEMY; ++i) {
        Tank& e = en[i];
        if (!e.alive) continue;
        if (e.spawn > 0) { if (--e.spawn == 0) e.inv = 30; continue; }
        if (e.inv > 0) --e.inv;
        if (freezeTimer > 0) continue;             // 时钟道具冻结

        float sp = (e.kind == 2) ? 1.1f : 0.7f;
        if (--e.mvTimer <= 0) { setDir(e, chooseDir(e)); e.mvTimer = 20 + rand() % 40; }

        bool moved = false;
        if (e.dir == DIR_UP)    moved = tryMove(e, 0, -sp);
        if (e.dir == DIR_DOWN)  moved = tryMove(e, 0, sp);
        if (e.dir == DIR_LEFT)  moved = tryMove(e, -sp, 0);
        if (e.dir == DIR_RIGHT) moved = tryMove(e, sp, 0);
        if (!moved) { setDir(e, chooseDir(e)); e.mvTimer = 10 + rand() % 20; }

        if (e.dir == DIR_UP)    tryMove(e, 0, -sp);
        if (e.dir == DIR_DOWN)  tryMove(e, 0, sp);
        if (e.dir == DIR_LEFT)  tryMove(e, -sp, 0);
        if (e.dir == DIR_RIGHT) tryMove(e, sp, 0);

        if (--e.fireTimer <= 0) {
            fire(e, 0, 1);
            int base = (e.kind == 4) ? 25 : 60;
            e.fireTimer = base + rand() % 60;
        }
    }
}

static void updateItems()
{
    for (int i = 0; i < MAX_ITEM; ++i) {
        Item& it = items[i];
        if (!it.alive) continue;
        bool take = false;
        if (player.alive && overlap(it.x, it.y, player.x, player.y)) take = true;
        if (p2On && player2.alive && overlap(it.x, it.y, player2.x, player2.y)) take = true;
        if (take) {
            it.alive = false;
            switch (it.type) {
                case IT_STAR:    if (starLv < 3) ++starLv; score += 200; break;
                case IT_GRENADE:
                    for (int k = 0; k < MAX_ENEMY; ++k)
                        if (en[k].alive) { addBoom(en[k].x, en[k].y); en[k].alive = false; score += 100; }
                    break;
                case IT_HELMET:  helmetTimer = 300; break;
                case IT_SHOVEL:  shovelTimer = 450;
                    for (int ty = 11; ty <= 12; ++ty)
                        for (int tx = 5; tx <= 7; ++tx)
                            if (map[ty][tx] == '#') map[ty][tx] = '@';
                    break;
                case IT_LIFE:    ++lives; break;
                case IT_CLOCK:   freezeTimer = 300; break;
            }
        }
    }
}

static void updateTimers()
{
    if (freezeTimer > 0) --freezeTimer;
    if (helmetTimer > 0) { --helmetTimer; player.inv = 2; if (p2On) player2.inv = 2; }
    if (shovelTimer > 0) {
        if (--shovelTimer == 0)
            for (int ty = 11; ty <= 12; ++ty)
                for (int tx = 5; tx <= 7; ++tx)
                    if (map[ty][tx] == '@' && !(tx == 6 && ty == 12)) map[ty][tx] = '#';
    }
}

static void spawnEnemies()
{
    if (enemyLeft <= 0) return;
    int onField = 0;
    for (int i = 0; i < MAX_ENEMY; ++i) if (en[i].alive) ++onField;
    if (onField >= 4) return;

    if (--spawnTimer > 0) return;
    spawnTimer = 90;

    static const int sx[3] = { 0, 5, 10 };
    int px = sx[nextSpawnIdx % 3];
    nextSpawnIdx++;

    for (int i = 0; i < MAX_ENEMY; ++i) {
        if (en[i].alive) continue;
        Tank& e = en[i];
        e.alive = true;
        e.x = (float)(px * TS);
        e.y = 0;
        e.dir = DIR_DOWN;
        e.kind = 1 + rand() % 4;
        e.hp = (e.kind == 3) ? 2 : 1;
        e.cd = 0;
        e.spawn = 24;
        e.inv = 0;
        e.mvTimer = 20 + rand() % 40;
        e.fireTimer = 60 + rand() % 60;
        e.bonus = (enemyLeft % 5 == 0);
        --enemyLeft;
        return;
    }
}

static void updateBooms()
{
    for (int i = 0; i < MAX_BOOM; ++i)
        if (booms[i].alive && ++booms[i].frame > 6) booms[i].alive = false;
}

static void update()
{
    ++tick;
    updatePlayer();
    updatePlayer2();
    updateEnemies();
    updateBullets();
    updateItems();
    updateBooms();
    updateTimers();
    spawnEnemies();

    if (enemyLeft == 0) {
        bool any = false;
        for (int i = 0; i < MAX_ENEMY; ++i) if (en[i].alive) any = true;
        if (!any) { state = ST_CLEAR; }
    }
}

// ---------------- 渲染 ----------------
static void renderScene()
{
    // 清屏
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < PW; ++x) pix[y][x] = C_BLACK;

    // 战场外框
    frect(FX - 1, FY - 1, FX + FIELD_W, FY - 1, C_DGRAY);
    frect(FX - 1, FY + FIELD_H, FX + FIELD_W, FY + FIELD_H, C_DGRAY);
    frect(FX - 1, FY - 1, FX - 1, FY + FIELD_H, C_DGRAY);
    frect(FX + FIELD_W, FY - 1, FX + FIELD_W, FY + FIELD_H, C_DGRAY);
    frect(FX, FY, FX + FIELD_W - 1, FY + FIELD_H - 1, C_BLACK);

    for (int ty = 0; ty < MH; ++ty)
        for (int tx = 0; tx < MW; ++tx)
            if (map[ty][tx] != '*') drawTile(tx, ty);

    for (int i = 0; i < MAX_ITEM; ++i) if (items[i].alive) drawItem(items[i]);
    for (int i = 0; i < MAX_ENEMY; ++i) if (en[i].alive) drawTank(en[i]);
    if (player.alive) drawTank(player);
    if (p2On && player2.alive) drawTank(player2);

    for (int ty = 0; ty < MH; ++ty)                 // 草地覆盖在坦克之上
        for (int tx = 0; tx < MW; ++tx)
            if (map[ty][tx] == '*') drawGrassOverlay(tx, ty);

    for (int i = 0; i < MAX_BULLET; ++i) {
        if (!bul[i].alive) continue;
        int px = FX + (int)bul[i].x, py = FY + (int)bul[i].y;
        frect(px, py, px + 1, py + 1, C_WHITE);
    }
    for (int i = 0; i < MAX_BOOM; ++i) if (booms[i].alive) drawBoom(booms[i]);
}

static void renderHud()
{
    char buf[64];
    drawText(28, 1, "1UP", C_RED);
    sprintf(buf, "%06d", score);       drawText(28, 2, buf, C_WHITE);
    drawText(28, 4, "HI-SCORE", C_YELLOW);
    sprintf(buf, "%06d", hiScore);     drawText(28, 5, buf, C_WHITE);

    drawText(28, 7, "STAGE", C_GRAY);
    sprintf(buf, "%d", level);         drawText(35, 7, buf, C_WHITE);
    drawText(28, 8, "ENEMY", C_GRAY);
    sprintf(buf, "%02d", enemyLeft);   drawText(35, 8, buf, C_WHITE);
    drawText(28, 9, "LIFE", C_GRAY);
    sprintf(buf, "%d", lives);         drawText(35, 9, buf, C_GREEN);

    if (p2On) {                                     // 联机时显示 2P 生命
        drawText(28, 10, "2P LIFE", C_GRAY);
        sprintf(buf, "%d", lives2);    drawText(36, 10, buf, C_YELLOW);
    }

    if (netMode != NET_NONE) {                      // 联机状态栏
        if (netMode == NET_HOST) {
            sprintf(buf, "HOST %s", netLocalIp());
            drawText(28, 17, buf, netOn ? C_GREEN : C_YELLOW);
            drawText(28, 18, netOn ? "P2 ONLINE" : "WAIT P2 JOIN", netOn ? C_GREEN : C_YELLOW);
        } else {
            drawText(28, 17, "CLIENT", netOn ? C_GREEN : C_YELLOW);
            drawText(28, 18, netOn ? "LINK OK" : "CONNECTING", netOn ? C_GREEN : C_YELLOW);
        }
    }

    drawText(28, 11, "PLAYER", C_GRAY);
    for (int i = 0; i < starLv; ++i) drawText(35 + i * 2, 11, "*", C_YELLOW);

    drawText(28, 13, "STATUS", C_GRAY);
    if (helmetTimer > 0) { sprintf(buf, "SHIELD %d", helmetTimer / 30); drawText(28, 14, buf, C_CYAN); }
    if (freezeTimer > 0) { sprintf(buf, "FREEZE %d", freezeTimer / 30); drawText(28, 15, buf, C_BLUE); }
    if (shovelTimer > 0) { sprintf(buf, "SHOVEL %d", shovelTimer / 30); drawText(28, 16, buf, C_BROWN); }

    drawText(28, 19, "MOVE  ARROWS/WASD", C_DGRAY);
    drawText(28, 20, "FIRE  SPACE / J", C_DGRAY);
    drawText(28, 21, "PAUSE P    RESET R", C_DGRAY);
    drawText(28, 22, "QUIT  Q", C_DGRAY);

    drawText(28, 24, "BRICK  BY 1 SHOT", C_BROWN);
    drawText(28, 25, "STEEL  BY LV3", C_GRAY);
    drawText(28, 26, "WATER  BLOCKS", C_BLUE);
    drawText(28, 27, "GRASS  HIDES", C_GREEN);
}

static void renderOverlay()
{
    char buf[64];
    if (state == ST_TITLE) {
        frect(FX, FY + 8, FX + FIELD_W - 1, FY + 34, C_BLACK);
        flushPixels();
        drawText(14, 7,  "T A N K   B A T T L E", C_YELLOW);
        drawText(12, 9,  "DEFEND THE EAGLE BASE", C_WHITE);
        drawText(10, 11, "ARROWS / WASD   MOVE", C_GRAY);
        drawText(10, 12, "SPACE / J       FIRE", C_GRAY);
        drawText(10, 13, "P PAUSE   R RESET   Q QUIT", C_GRAY);
        drawText(13, 15, "PRESS SPACE TO START", C_CYAN);
        drawText(9, 17, "LAN  tank.exe host", C_DGRAY);
        drawText(9, 18, "     tank.exe join <IP>", C_DGRAY);
        if (netMode == NET_HOST) {
            sprintf(buf, "MY IP  %s", netLocalIp());
            drawText(9, 19, buf, netOn ? C_GREEN : C_YELLOW);
        } else if (netMode == NET_CLIENT) {
            drawText(9, 19, netOn ? "LINKED - HOST STARTS" : "CONNECTING...", C_YELLOW);
        }
    }
    if (state == ST_PAUSE) {
        drawText(15, 12, "P A U S E D", C_WHITE);
    }
    if (state == ST_CLEAR) {
        drawText(13, 11, "STAGE CLEAR !", C_YELLOW);
        sprintf(buf, "STAGE %d COMPLETE", level);
        drawText(11, 13, buf, C_WHITE);
        drawText(11, 15, "PRESS SPACE FOR NEXT", C_CYAN);
    }
    if (state == ST_OVER) {
        drawText(14, 11, "G A M E   O V E R", C_RED);
        sprintf(buf, "SCORE %d", score);
        drawText(16, 13, buf, C_WHITE);
        drawText(12, 15, "R  RETRY      Q  QUIT", C_GRAY);
    }
}

static void render()
{
    renderScene();
    flushPixels();
    renderHud();
    renderOverlay();
    present();
}

// ---------------- 键盘（ReadConsoleInput，kernel32 即可） ----------------
static void pollInput()
{
    memset(keyHit, 0, sizeof(keyHit));
    DWORD pending = 0;
    while (GetNumberOfConsoleInputEvents(hIn, &pending) && pending > 0) {
        INPUT_RECORD rec[16];
        DWORD got = 0;
        if (!ReadConsoleInputW(hIn, rec, 16, &got) || got == 0) break;
        for (DWORD i = 0; i < got; ++i) {
            if (rec[i].EventType != KEY_EVENT) continue;
            const KEY_EVENT_RECORD& k = rec[i].Event.KeyEvent;
            WORD vk = k.wVirtualKeyCode;
            if (vk >= 256) continue;
            if (k.bKeyDown) { keyDown[vk] = true; keyHit[vk] = true; }
            else            { keyDown[vk] = false; }
        }
    }
}

// ---------------- 主流程 ----------------
static const char* gJoinIp = NULL;

static bool streqi(const char* a, const char* b)
{
    while (*a && *b) {
        char c1 = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char c2 = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (c1 != c2) return false;
        ++a; ++b;
    }
    return *a == *b;
}

static void spawnP2()
{
    p2On = true;
    lives2 = 3;
    if (state != ST_PLAY) { player2.alive = false; return; }
    player2.alive = true;
    player2.x = 9 * TS; player2.y = 11 * TS;
    player2.dir = DIR_UP; player2.kind = 0; player2.hp = 1; player2.cd = 0;
    player2.spawn = 0; player2.inv = 90; player2.bonus = false;
}

static void dropP2()
{
    p2On = false;
    player2.alive = false;
}

static void hostNetFrame()
{
    if (netAcceptPoll()) { spawnP2(); return; }            // 有人接入
    if (!netOn) { if (p2On) dropP2(); return; }            // 掉线
    if (netPump() < 0) { dropP2(); return; }
    PktInput in;
    while (netTake(&in, sizeof(in)))
        if (in.type == PKT_INPUT) { c2dir = in.dir; c2fire = in.fire != 0; }
}

static void clientNetFrame()
{
    if (!netOn) {                                          // 断线重连
        if (!netConnecting && (int)(GetTickCount() - netRetry) > 1500) {
            netRetry = GetTickCount();
            netConnectStart(gJoinIp);
        }
        netConnectPoll();
        return;
    }
    PktInput in;
    memset(&in, 0, sizeof(in));
    in.type = PKT_INPUT;
    in.dir  = c1dir;
    in.fire = c1fire ? 1 : 0;
    netSendAll(&in, sizeof(in));

    if (netPump() < 0) return;
    PktSnap s;
    while (netTake(&s, sizeof(s)))
        if (s.type == PKT_SNAP) applySnap(s);
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (streqi(argv[i], "host")) {
            netMode = NET_HOST;
        } else if (streqi(argv[i], "join") || streqi(argv[i], "client")) {
            if (i + 1 >= argc) { printf("用法: tank.exe join <主机IP>\n"); return 1; }
            netMode = NET_CLIENT;
            gJoinIp = argv[++i];
        }
    }

    if (netMode != NET_NONE) {
        if (!netStartup()) { printf("WSAStartup failed\n"); return 1; }
    }
    if (netMode == NET_HOST && !netListen(NET_PORT)) {
        printf("监听 %d 端口失败（可能已被占用）\n", NET_PORT);
        return 1;
    }

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn  = GetStdHandle(STD_INPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(hOut, &ci)) { ci.bVisible = FALSE; SetConsoleCursorInfo(hOut, &ci); }
    if (netMode == NET_HOST)        SetConsoleTitleW(L"Tank Battle  [HOST]");
    else if (netMode == NET_CLIENT) SetConsoleTitleW(L"Tank Battle  [CLIENT]");
    else                            SetConsoleTitleW(L"Tank Battle");

    if (netMode == NET_CLIENT) netConnectStart(gJoinIp);

    srand((unsigned)time(NULL));
    hiScore = 0;
    score = 0; lives = 3; level = 1; starLv = 1;
    baseAlive = true;
    state = ST_TITLE;

    const int TICK = 33;
    DWORD nextTick = GetTickCount();
    bool quit = false;

    while (!quit) {
        pollInput();
        if (keyHit['Q']) quit = true;
        readLocalCtrl(c1dir, c1fire);

        if (netMode == NET_CLIENT) {                 // 客户端：只发按键、收快照
            clientNetFrame();
            render();
            Sleep(6);
            continue;
        }

        if (netMode == NET_HOST) hostNetFrame();     // 收 2P 按键

        if (!quit) {
            if (state == ST_TITLE) {
                if (keyHit[VK_SPACE]) { resetGame(); nextTick = GetTickCount() + TICK; }
            } else if (state == ST_OVER) {
                if (keyHit['R']) { resetGame(); nextTick = GetTickCount() + TICK; }
            } else if (state == ST_CLEAR) {
                if (keyHit[VK_SPACE]) {
                    ++level;
                    loadLevel(level);
                    state = ST_PLAY;
                    nextTick = GetTickCount() + TICK;
                }
            } else if (keyHit['P']) {
                state = (state == ST_PAUSE) ? ST_PLAY : ST_PAUSE;
            }
        }

        if (!quit && state == ST_PLAY) {
            DWORD now = GetTickCount();
            while (now >= nextTick) {
                update();
                nextTick += TICK;
                if (state != ST_PLAY) break;
            }
            if (score > hiScore) hiScore = score;
        }

        if (netMode == NET_HOST && netOn) {          // 每帧推一份完整世界给客户端
            PktSnap s;
            packSnap(s);
            netSendAll(&s, sizeof(s));
        }

        render();
        Sleep(6);
    }

    netDrop();
    if (netMode != NET_NONE) WSACleanup();
    if (GetConsoleCursorInfo(hOut, &ci)) { ci.bVisible = TRUE; SetConsoleCursorInfo(hOut, &ci); }
    return 0;
}
