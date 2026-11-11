#include "pch.h"
#include "FixDecisions.h"

namespace FixDecisions {

bool clampStringRead(const unsigned char* data, int len, int pos, short* lengthOut) {
    if (!data || pos < 0 || pos + 2 > len) return false;             // not enough data for the length
    short* p = (short*)(data + pos);
    int rest = len - pos - 2;
    if (*p < 0) { *p = 0; if (lengthOut) *lengthOut = *p; return true; }
    else if (*p > rest) { *p = (short)rest; if (lengthOut) *lengthOut = *p; return true; }
    if (lengthOut) *lengthOut = *p;
    return false;
}

bool checkTransferPath(GameString* s, const std::string& docsPrefix, long* blockedCount) {
    if (!s || docsPrefix.empty() || s->size == 0 || s->size > 4096) return false;
    char* d = gsData(s);
    std::string p(d, s->size);
    std::string low = p;
    for (auto& c : low) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; }
    if (low.compare(0, docsPrefix.size(), docsPrefix) != 0) return false;   // game's own local path
    std::string rest = low.substr(docsPrefix.size());
    bool bad = rest.find("..") != std::string::npos || rest.find(':') != std::string::npos ||
               (!rest.empty() && rest[0] == '\\');
    if (bad) {
        if (blockedCount) InterlockedIncrement(blockedCount);
        d[0] = 0;
        s->size = 0;
    }
    return bad;
}

} // namespace FixDecisions
