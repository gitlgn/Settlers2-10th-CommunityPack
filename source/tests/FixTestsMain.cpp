// Headless unit tests for FixDecisions - the pure decision logic the live d3d9.dll patch uses
// for the savegame/map string-length clamp and the multiplayer TFTP path-traversal block.
//
// Runs without the game, without a second LAN player and without a live network transfer: it
// feeds the same byte patterns and GameString layouts the real hook would see. Linked against
// the production FixDecisions.cpp, so it tests the logic that actually ships - not a replica.
//
// Build: cmake --build build --target FixTests     Run: build\bin\FixTests.exe
// Exit code 0 = all passed, non-zero = failure count.

#include "pch.h"
#include "FixDecisions.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0, g_pass = 0;
static const char* g_cur = "";

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  [FAIL] %s: %s\n", g_cur, msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { g_pass++; } \
    else { g_fail++; printf("  [FAIL] %s: %s (got %d, want %d)\n", g_cur, msg, (int)_a, (int)_b); } \
} while (0)

static void section(const char* name) { g_cur = name; }

// --------------------------------------------------------------------------- clampStringRead

static void testClampStringRead() {
    section("clampStringRead");

    // --- legitimate: short string fully within the remaining data ---
    {
        unsigned char buf[16] = {4, 0, 'A', 'B', 'C', 'D', 'X', 'X', 'X'};
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 6, 0, &len);
        CHECK(!clamped, "legit short not clamped");
        CHECK_EQ(len, 4, "legit short length preserved");
        CHECK(buf[0] == 4 && buf[1] == 0, "legit short bytes untouched");
    }

    // --- hostile: length exceeds remaining bytes -> clamped down to rest ---
    {
        unsigned char buf[16] = {0x10, 0x00, 'A', 'B', 'C', 'D'};   // len=16, only 4 bytes left
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 6, 0, &len);
        CHECK(clamped, "oversized length clamped");
        CHECK_EQ(len, 4, "oversized clamped to remaining");
    }

    // --- hostile: negative length (top bit set) -> forced to 0 ---
    {
        unsigned char buf[16] = {0xFF, 0xFF, 'A', 'B'};             // len = -1
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 6, 0, &len);
        CHECK(clamped, "negative length clamped");
        CHECK_EQ(len, 0, "negative length forced to 0");
    }

    // --- hostile: negative 16-bit length (high byte 0xFF) ---
    {
        unsigned char buf[16] = {0x80, 0xFF, 'A', 'B', 'C', 'D'};  // len = 0xFF80 = -128 (as an unsigned 16-bit length: ~64 KB)
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 8, 0, &len);
        CHECK(clamped, "negative-32768 length clamped");
        CHECK_EQ(len, 0, "negative-32768 forced to 0");
    }

    // --- boundary: length exactly equals remaining bytes -> NOT clamped ---
    {
        unsigned char buf[16] = {4, 0, 'A', 'B', 'C', 'D'};        // len=4, rest=4
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 6, 0, &len);
        CHECK(!clamped, "exact-fit length not clamped");
        CHECK_EQ(len, 4, "exact-fit length preserved");
    }

    // --- boundary: length one byte over remaining -> clamped down by one ---
    {
        unsigned char buf[16] = {5, 0, 'A', 'B', 'C', 'D', 'X'};   // len=5, rest=4 (len at 0..1, bytes at 2..5)
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 6, 0, &len);
        CHECK(clamped, "len=rest+1 clamped");
        CHECK_EQ(len, 4, "len=rest+1 clamped to rest");
    }

    // --- length zero (empty string) -> not clamped, len stays 0 ---
    {
        unsigned char buf[16] = {0, 0, 'A'};
        short len = 99;
        bool clamped = FixDecisions::clampStringRead(buf, 3, 0, &len);
        CHECK(!clamped, "zero length not clamped");
        CHECK_EQ(len, 0, "zero length preserved");
    }

    // --- too short for a 16-bit length -> untouched, not clamped ---
    {
        unsigned char buf[16] = {1, 0};
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 1, 0, &len);
        CHECK(!clamped, "no data not clamped");

        unsigned char buf2[16] = {5, 0};
        clamped = FixDecisions::clampStringRead(buf2, 1, 0, &len);
        CHECK(!clamped, "single byte not clamped");

        clamped = FixDecisions::clampStringRead(nullptr, 8, 0, &len);
        CHECK(!clamped, "null data not clamped");
    }

    // --- pos not at start: length lives at pos=4 ---
    {
        unsigned char buf[16] = {'x', 'x', 'x', 'x', 3, 0, 'A', 'B', 'C'};
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 9, 4, &len);
        CHECK(!clamped, "mid-stream length not clamped");
        CHECK_EQ(len, 3, "mid-stream length preserved");

        buf[4] = 8;  // now len=8, rest = 9-4-2 = 3 -> clamped to 3
        clamped = FixDecisions::clampStringRead(buf, 9, 4, &len);
        CHECK(clamped, "mid-stream oversized clamped");
        CHECK_EQ(len, 3, "mid-stream oversized clamped to rest");
    }

    // --- pos beyond the data -> untouched ---
    {
        unsigned char buf[16] = {0, 0, 0};
        short len;
        bool clamped = FixDecisions::clampStringRead(buf, 3, 2, &len);
        CHECK(!clamped, "pos at end not clamped (needs 2 bytes past)");

        clamped = FixDecisions::clampStringRead(buf, 3, 3, &len);
        CHECK(!clamped, "pos past end not clamped");

        clamped = FixDecisions::clampStringRead(buf, 3, -1, &len);
        CHECK(!clamped, "negative pos not clamped");
    }
}

// ----------------------------------------------------------------------- checkTransferPath

static FixDecisions::GameString makeGameString(const char* text) {
    FixDecisions::GameString s;
    memset(&s, 0, sizeof s);
    std::string t(text);
    if (t.size() >= 16) {
        s.u.ptr = (char*)malloc(t.size() + 1);
        memcpy(s.u.ptr, t.c_str(), t.size() + 1);
        s.cap = 4096;
    } else {
        memcpy(s.u.buf, t.c_str(), t.size() + 1);
        s.cap = 16;
    }
    s.size = (unsigned int)t.size();
    return s;
}

static void testCheckTransferPath() {
    section("checkTransferPath");
    const std::string docs = "c:\\users\\maber\\documents\\settlers 2\\";  // lower-cased, trailing sep
    long blocked = 0;

    // --- legitimate: same folder, plain file name -> NOT blocked ---
    {
        auto s = makeGameString("c:\\users\\maber\\documents\\settlers 2\\save1.sav");
        CHECK(!FixDecisions::checkTransferPath(&s, docs, &blocked), "legit same-folder not blocked");
        CHECK_EQ((int)s.size, (int)std::string("c:\\users\\maber\\documents\\settlers 2\\save1.sav").size(),
                 "legit path untouched");
    }

    // --- legitimate: subfolder inside documents -> NOT blocked ---
    {
        auto s = makeGameString("c:\\users\\maber\\documents\\settlers 2\\sub\\a.map");
        CHECK(!FixDecisions::checkTransferPath(&s, docs, &blocked), "legit subfolder not blocked");
    }

    // --- hostile: '..' to escape -> blocked, path emptied, count bumped ---
    {
        long before = blocked;
        auto s = makeGameString("c:\\users\\maber\\documents\\settlers 2\\..\\..\\windows\\win.ini");
        CHECK(FixDecisions::checkTransferPath(&s, docs, &blocked), "dotdot escape blocked");
        CHECK_EQ((int)blocked, before + 1, "dotdot bump count");
        CHECK_EQ((int)s.size, 0, "dotdot path emptied");
    }

    // --- hostile: drive letter colon -> blocked ---
    {
        long before = blocked;
        auto s = makeGameString("c:\\users\\maber\\documents\\settlers 2\\evil:C:\\x.sav");
        CHECK(FixDecisions::checkTransferPath(&s, docs, &blocked), "colon escape blocked");
        CHECK_EQ((int)blocked, before + 1, "colon bump count");
    }

    // --- hostile: multi dotdot straight into system32 -> blocked ---
    {
        long before = blocked;
        auto s = makeGameString("c:\\users\\maber\\documents\\settlers 2\\..\\..\\..\\windows\\system32\\x.dll");
        CHECK(FixDecisions::checkTransferPath(&s, docs, &blocked), "multi-dotdot escape blocked");
        CHECK_EQ((int)blocked, before + 1, "multi-dotdot bump count");
    }

    // --- not under the documents prefix (game's own local save) -> NOT blocked, untouched ---
    {
        long before = blocked;
        auto s = makeGameString("d:\\games\\settlers 10\\local\\my save.sav");
        CHECK(!FixDecisions::checkTransferPath(&s, docs, &blocked), "off-prefix local path not blocked");
        CHECK_EQ((int)blocked, before, "off-prefix count unchanged");
    }

    // --- case-insensitive: prefix given lower, path has uppercase -> still recognised & checked ---
    {
        long before = blocked;
        auto s = makeGameString("C:\\Users\\Maber\\Documents\\Settlers 2\\..\\..\\secret");
        CHECK(FixDecisions::checkTransferPath(&s, docs, &blocked), "mixed-case prefix still blocked");
        CHECK_EQ((int)blocked, before + 1, "mixed-case bump count");
    }

    // --- slash separators are normalised to backslash before checking ---
    {
        long before = blocked;
        auto s = makeGameString("c:\\users\\maber\\documents\\settlers 2/../../etc/passwd");
        CHECK(FixDecisions::checkTransferPath(&s, docs, &blocked), "slash dotdot blocked");
        CHECK_EQ((int)blocked, before + 1, "slash dotdot bump count");
    }

    // --- null string -> no crash, not blocked ---
    {
        long before = blocked;
        CHECK(!FixDecisions::checkTransferPath(nullptr, docs, &blocked), "null path not blocked");
        CHECK_EQ((int)blocked, before, "null count unchanged");
    }

    // --- empty docs prefix -> not blocked, no crash ---
    {
        long before = blocked;
        auto s = makeGameString("c:\\x\\..\\..\\win.ini");
        std::string empty;
        CHECK(!FixDecisions::checkTransferPath(&s, empty, &blocked), "empty prefix not blocked");
        CHECK_EQ((int)blocked, before, "empty prefix count unchanged");
    }
}

// --------------------------------------------------------------------------- main

int main() {
    testClampStringRead();
    testCheckTransferPath();

    printf("\nFixDecisions tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    return g_fail;
}