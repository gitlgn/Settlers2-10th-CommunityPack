#include "pch.h"
#include "FixDecisions.h"
#include <cstdlib>

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

float parseIniFloat(const char* text, float def) {
    if (!text) return def;
    char buf[64];
    size_t n = 0;
    while (*text == ' ' || *text == '\t') text++;
    for (; *text && n < sizeof(buf) - 1; text++) buf[n++] = (*text == ',') ? '.' : *text;
    buf[n] = 0;
    char* end = nullptr;
    double v = strtod(buf, &end);   // the DLL never changes the C locale: '.' is the decimal separator
    if (end == buf) return def;
    return (float)v;
}

} // namespace FixDecisions
