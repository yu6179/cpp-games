// ============================================================
//  俄罗斯方块 Tetris  ——  Windows 控制台单文件版
//  编译：
//    Dev-C++ / Code::Blocks：直接新建控制台项目粘贴本文件，F11 编译运行
//    MinGW g++：  g++ tetris.cpp -o tetris.exe -O2
//    MSVC cl ：   cl tetris.cpp /EHsc
//  运行：双击生成的 tetris.exe（控制台窗口建议至少 40 列 x 24 行）
//  操作：A/D 或 ←/→ 左右移动    W/↑ 顺时针旋转    Z 逆时针旋转
//        S/↓ 加速下落    空格 直接落底    P 暂停    R 重开    Q 退出
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// ---------------- 尺寸 ----------------
enum { BW = 10, BH = 20, VW = 42, VH = 22 };   // 棋盘 10x20，虚拟屏幕 42x22

// ---------------- 七种方块：7 种 x 4 个旋转态 x 4 个格子 x (x,y) ----------------
static const int PIECES[7][4][4][2] = {
    {   // 0: I
        {{0,1},{1,1},{2,1},{3,1}},
        {{2,0},{2,1},{2,2},{2,3}},
        {{0,2},{1,2},{2,2},{3,2}},
        {{1,0},{1,1},{1,2},{1,3}}
    },
    {   // 1: O
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{2,1}}
    },
    {   // 2: T
        {{1,0},{0,1},{1,1},{2,1}},
        {{1,0},{1,1},{2,1},{1,2}},
        {{0,1},{1,1},{2,1},{1,2}},
        {{1,0},{0,1},{1,1},{1,2}}
    },
    {   // 3: S
        {{1,0},{2,0},{0,1},{1,1}},
        {{1,0},{1,1},{2,1},{2,2}},
        {{1,1},{2,1},{0,2},{1,2}},
        {{1,0},{1,1},{2,1},{2,2}}
    },
    {   // 4: Z
        {{0,0},{1,0},{1,1},{2,1}},
        {{2,0},{1,1},{2,1},{1,2}},
        {{0,1},{1,1},{1,2},{2,2}},
        {{2,0},{1,1},{2,1},{1,2}}
    },
    {   // 5: J
        {{0,0},{0,1},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{1,2}},
        {{0,1},{1,1},{2,1},{2,2}},
        {{1,0},{1,1},{0,2},{1,2}}
    },
    {   // 6: L
        {{2,0},{0,1},{1,1},{2,1}},
        {{1,0},{1,1},{1,2},{2,2}},
        {{0,1},{1,1},{2,1},{0,2}},
        {{0,0},{1,0},{1,1},{1,2}}
    }
};

// ---------------- 全局状态 ----------------
static int  board[BH][BW];          // 0=空，1..7=已固定的方块颜色
static int  px, py, rot;            // 当前方块位置与旋转态
static int  curType, nextType;
static int  score, clearedLines, level;
static bool gameOver, paused, quitGame;

static HANDLE    hOut;
static CHAR_INFO vbuf[VH][VW];      // 帧缓冲，一次性刷屏，不闪烁

static int  bag[7], bagPos = 7;     // 7-bag 随机序列

// ---------------- 颜色 ----------------
static const WORD C_FRAME = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; // 青
static const WORD C_TEXT  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; // 白
static const WORD C_LABEL = FOREGROUND_GREEN | FOREGROUND_INTENSITY;  // 亮绿
static const WORD C_GRID  = FOREGROUND_INTENSITY;                     // 暗灰

static WORD pieceColor(int t)
{
    switch (t) {
        case 1: return BACKGROUND_GREEN | BACKGROUND_BLUE  | BACKGROUND_INTENSITY; // I 青
        case 2: return BACKGROUND_RED   | BACKGROUND_GREEN | BACKGROUND_INTENSITY; // O 黄
        case 3: return BACKGROUND_RED   | BACKGROUND_BLUE  | BACKGROUND_INTENSITY; // T 紫
        case 4: return BACKGROUND_GREEN | BACKGROUND_INTENSITY;                    // S 绿
        case 5: return BACKGROUND_RED   | BACKGROUND_INTENSITY;                    // Z 红
        case 6: return BACKGROUND_BLUE  | BACKGROUND_INTENSITY;                    // J 蓝
        default:return BACKGROUND_RED   | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY; // L 白
    }
}

// ---------------- 画面基础 ----------------
static void putCell(int x, int y, wchar_t c, WORD attr)
{
    if (x < 0 || x >= VW || y < 0 || y >= VH) return;
    vbuf[y][x].Char.UnicodeChar = c;
    vbuf[y][x].Attributes       = attr;
}

static void drawText(int x, int y, const char* s, WORD attr)
{
    for (int i = 0; s[i]; ++i) putCell(x + i, y, (wchar_t)(unsigned char)s[i], attr);
}

static void fillCell(int x, int y, WORD attr)   // 一个方块 = 2 个字符宽
{
    putCell(x, y, L' ', attr);
    putCell(x + 1, y, L' ', attr);
}

static void present()
{
    COORD      size = { (SHORT)VW, (SHORT)VH };
    COORD      from = { 0, 0 };
    SMALL_RECT dst  = { 0, 0, (SHORT)(VW - 1), (SHORT)(VH - 1) };

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {   // 跟随当前视口左上角，避免滚屏错位
        dst.Left   = csbi.srWindow.Left;
        dst.Top    = csbi.srWindow.Top;
        dst.Right  = (SHORT)(dst.Left + VW - 1);
        dst.Bottom = (SHORT)(dst.Top  + VH - 1);
    }
    WriteConsoleOutputW(hOut, &vbuf[0][0], size, from, &dst);
}

// ---------------- 规则逻辑 ----------------
static bool collides(int type, int r, int x0, int y0)
{
    for (int i = 0; i < 4; ++i) {
        int x = x0 + PIECES[type][r][i][0];
        int y = y0 + PIECES[type][r][i][1];
        if (x < 0 || x >= BW || y >= BH) return true;
        if (y >= 0 && board[y][x]) return true;
    }
    return false;
}

static int nextFromBag()
{
    if (bagPos >= 7) {
        for (int i = 0; i < 7; ++i) bag[i] = i;
        for (int i = 6; i > 0; --i) {
            int j = rand() % (i + 1);
            int t = bag[i]; bag[i] = bag[j]; bag[j] = t;
        }
        bagPos = 0;
    }
    return bag[bagPos++];
}

static void spawnPiece()
{
    curType  = nextType;
    nextType = nextFromBag();
    rot = 0; px = 3; py = 0;
    if (collides(curType, 0, px, py)) gameOver = true;
}

static void clearLines()
{
    int n = 0;
    for (int y = BH - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < BW; ++x) if (!board[y][x]) { full = false; break; }
        if (full) {
            ++n;
            for (int yy = y; yy > 0; --yy)
                for (int x = 0; x < BW; ++x) board[yy][x] = board[yy - 1][x];
            for (int x = 0; x < BW; ++x) board[0][x] = 0;
            ++y;                       // 同一行需要重新检查
        }
    }
    if (n) {
        static const int tbl[5] = { 0, 100, 300, 500, 800 };
        score       += tbl[n] * level;
        clearedLines += n;
        level        = clearedLines / 10 + 1;
        if (level > 20) level = 20;
    }
}

static void lockPiece()
{
    for (int i = 0; i < 4; ++i) {
        int x = px + PIECES[curType][rot][i][0];
        int y = py + PIECES[curType][rot][i][1];
        if (y >= 0 && y < BH && x >= 0 && x < BW) board[y][x] = curType + 1;
    }
    clearLines();
    spawnPiece();
}

static bool tryMove(int dx, int dy)
{
    if (!collides(curType, rot, px + dx, py + dy)) {
        px += dx; py += dy;
        return true;
    }
    if (dy == 1) lockPiece();           // 撞到底部即锁定
    return false;
}

static void rotatePiece(int dir)
{
    int nr = (rot + (dir > 0 ? 1 : 3)) % 4;
    const int kick[7][2] = { {0,0}, {-1,0}, {1,0}, {-2,0}, {2,0}, {0,-1}, {0,1} };
    for (int i = 0; i < 7; ++i) {
        int nx = px + kick[i][0];
        int ny = py + kick[i][1];
        if (!collides(curType, nr, nx, ny)) { rot = nr; px = nx; py = ny; return; }
    }
}

static void hardDrop()
{
    while (!collides(curType, rot, px, py + 1)) { ++py; score += 2; }
    lockPiece();
}

static int dropInterval()
{
    int v = 800 - (level - 1) * 70;
    return v < 80 ? 80 : v;
}

static void newGame()
{
    memset(board, 0, sizeof(board));
    score = 0; clearedLines = 0; level = 1;
    gameOver = false; paused = false;
    bagPos = 7;
    nextType = nextFromBag();
    spawnPiece();
}

// ---------------- 绘制一帧 ----------------
static void drawFrame()
{
    // 清屏（黑色）
    for (int y = 0; y < VH; ++y)
        for (int x = 0; x < VW; ++x) { vbuf[y][x].Char.UnicodeChar = L' '; vbuf[y][x].Attributes = 0; }

    // 边框（Unicode 制表符，用转义写法避免编码问题）
    putCell(0, 0, L'\u2554', C_FRAME);
    for (int x = 1; x <= BW * 2; ++x) putCell(x, 0, L'\u2550', C_FRAME);
    putCell(BW * 2 + 1, 0, L'\u2557', C_FRAME);
    putCell(0, BH + 1, L'\u255A', C_FRAME);
    for (int x = 1; x <= BW * 2; ++x) putCell(x, BH + 1, L'\u2550', C_FRAME);
    putCell(BW * 2 + 1, BH + 1, L'\u255D', C_FRAME);
    for (int y = 1; y <= BH; ++y) {
        putCell(0, y, L'\u2551', C_FRAME);
        putCell(BW * 2 + 1, y, L'\u2551', C_FRAME);
    }

    // 已固定的方块
    for (int y = 0; y < BH; ++y)
        for (int x = 0; x < BW; ++x) {
            int sx = 1 + x * 2, sy = 1 + y;
            if (board[y][x]) {
                fillCell(sx, sy, pieceColor(board[y][x]));
            } else {
                putCell(sx, sy, L'.', C_GRID);
                putCell(sx + 1, sy, L' ', C_GRID);
            }
        }

    // 当前方块
    for (int i = 0; i < 4; ++i) {
        int bx = px + PIECES[curType][rot][i][0];
        int by = py + PIECES[curType][rot][i][1];
        if (by >= 0) fillCell(1 + bx * 2, 1 + by, pieceColor(curType + 1));
    }

    // 右侧信息栏
    char buf[32];
    drawText(23, 1, "SCORE", C_LABEL);
    sprintf(buf, "%d", score);          drawText(23, 2, buf, C_TEXT);
    drawText(23, 4, "LEVEL", C_LABEL);
    sprintf(buf, "%d", level);          drawText(23, 5, buf, C_TEXT);
    drawText(23, 7, "LINES", C_LABEL);
    sprintf(buf, "%d", clearedLines);   drawText(23, 8, buf, C_TEXT);

    drawText(23, 10, "NEXT", C_LABEL);
    for (int i = 0; i < 4; ++i) {
        int bx = PIECES[nextType][0][i][0];
        int by = PIECES[nextType][0][i][1];
        fillCell(24 + bx * 2, 12 + by, pieceColor(nextType + 1));
    }

    drawText(23, 16, "A / D  MOVE",  C_TEXT);
    drawText(23, 17, "W      ROTATE", C_TEXT);
    drawText(23, 18, "S      SOFT",   C_TEXT);
    drawText(23, 19, "SPACE  DROP",   C_TEXT);
    drawText(23, 20, "P PAUSE  R RESET", C_TEXT);
    drawText(23, 21, "Q QUIT",        C_TEXT);

    // 暂停 / 结束覆盖层
    if (paused && !gameOver) {
        drawText(6, 9,  "  PAUSED  ",  BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    }
    if (gameOver) {
        WORD inv = BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        drawText(4, 8,  "  GAME OVER  ", inv);
        drawText(4, 9,  " R  RESTART  ", inv);
        drawText(4, 10, " Q  QUIT     ", inv);
    }
}

// ---------------- 键盘 ----------------
static int readKey()
{
    if (!_kbhit()) return -1;
    int c = _getch();
    if (c == 0 || c == 224) {           // 方向键等扩展键
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
    SetConsoleTitleW(L"Tetris");

    srand((unsigned)time(NULL));
    newGame();

    DWORD lastDrop = GetTickCount();
    while (!quitGame) {
        int k;
        while ((k = readKey()) != -1) {
            if (k == 'q' || k == 'Q') { quitGame = true; break; }

            if (gameOver) {
                if (k == 'r' || k == 'R') { newGame(); lastDrop = GetTickCount(); }
                continue;
            }
            if (k == 'p' || k == 'P') { paused = !paused; continue; }
            if (paused) continue;

            switch (k) {
                case 1000 + 75: case 'a': case 'A': tryMove(-1, 0); break;
                case 1000 + 77: case 'd': case 'D': tryMove( 1, 0); break;
                case 1000 + 80: case 's': case 'S':
                    if (tryMove(0, 1)) score += 1;
                    lastDrop = GetTickCount();
                    break;
                case 1000 + 72: case 'w': case 'W': case 'x': case 'X': rotatePiece( 1); break;
                case 'z': case 'Z':                                          rotatePiece(-1); break;
                case ' ':                                                    hardDrop();
                    lastDrop = GetTickCount();
                    break;
                default: break;
            }
        }
        if (quitGame) break;

        if (!gameOver && !paused) {
            DWORD now = GetTickCount();
            if (now - lastDrop >= (DWORD)dropInterval()) {
                lastDrop = now;
                tryMove(0, 1);
            }
        }

        drawFrame();
        present();
        Sleep(16);
    }

    if (GetConsoleCursorInfo(hOut, &ci)) { ci.bVisible = TRUE; SetConsoleCursorInfo(hOut, &ci); }
    return 0;
}
