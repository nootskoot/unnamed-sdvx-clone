#pragma once
#include <string>
// C++17-facing API; the official parser is built separately as C++20.
bool ConvertKSH2(const std::string& input, std::string& output, std::string& diagnostics);
