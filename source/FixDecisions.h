#pragma once

// Pure decision logic of the d3d9.dll-Proxy binary fixes. No patching, no hooks, no
// process globals: everything needed comes in as parameters, so the same functions the live
// patch uses can be unit-tested headlessly - without the game, a second LAN player or a live
// network transfer. See source/tests for the tests.

#include <cstddef>
#include <string>

namespace FixDecisions {

// Game-internal std::string (Siedler2 "std" build): first 16 chars inline, otherwise a heap
// pointer. cap >= 16 means the data lives at u.ptr, otherwise in the inline buf[16].
struct GameString {
    unsigned int alloc;
    union { char buf[16]; char* ptr; } u;
    unsigned int size;
    unsigned int cap;
};

inline char* gsData(GameString* s) { return s->cap >= 16 ? s->u.ptr : s->u.buf; }

// One call clamps the 16-bit string length a savegame/map stream hands us, before the original
// reader runs. data/len/pos mirror the game's memory stream: bytes [pos, pos+2) hold the signed
// 16-bit length, then the characters. A negative length (-> ~4 GB) or one past the remaining
// data is what let a crafted file overflow the read buffer, so it is forced into
// [0, remaining bytes]. Returns true if a value was clamped (the data looked hostile).
bool clampStringRead(const unsigned char* data, int len, int pos, short* lengthOut);

// Own-multiplayer file transfer (TFTP, message type 300). The game builds the requested path as
// "<documents>\" + name and opens/sends it. Given the lower-cased, trailing-separator documents
// prefix exactly as the live hook stores it, decide whether the part after it may escape the
// folder: "..", a drive/colon, or a leading separator. On a hostile request the path is emptied
// in place (the game then answers TFTP_ERROR_FILENOTFOUND); on a legitimate path - or one that
// is not under the documents prefix (the game's own local save) - nothing is touched.
// Returns true if the request was blocked.
bool checkTransferPath(GameString* path, const std::string& docsPrefix, long* blockedCount);

} // namespace FixDecisions
