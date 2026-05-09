#pragma once
#include <string>
#include <algorithm>

inline std::string toLower(const std::string& s)
{
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}