// 中文：摘自 CrossMux src/util/StringUtils.cpp 的 sanitizeFilename 和 lib/Utf8 的 utf8NextCodepoint（MIT，见
// LICENSE-CrossMux.txt），合成一个头文件；书名到文件名的规则不变，已下载的书路径保持一致。
// English: sanitizeFilename from CrossMux src/util/StringUtils.cpp and utf8NextCodepoint from lib/Utf8 (MIT, see
// LICENSE-CrossMux.txt), merged into one header; the title-to-file-name rule is unchanged so download paths stay put.

#pragma once

#include <cstdint>
#include <string>

namespace StringUtils {

namespace detail {

inline int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

inline uint32_t utf8NextCodepoint(const unsigned char** string) {
  constexpr uint32_t kReplacement = 0xFFFD;
  if (**string == 0) return 0;
  const unsigned char lead = **string;
  const int bytes = utf8CodepointLen(lead);
  const uint8_t* chr = *string;
  if (bytes == 1 && lead >= 0x80) {
    (*string)++;
    return kReplacement;
  }
  if (bytes == 1) {
    (*string)++;
    return chr[0];
  }
  for (int i = 1; i < bytes; i++) {
    if ((chr[i] & 0xC0) != 0x80) {
      *string += i;
      return kReplacement;
    }
  }
  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);
  for (int i = 1; i < bytes; i++) cp = (cp << 6) | (chr[i] & 0x3F);
  const bool overlong = (bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000);
  const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
  if (overlong || surrogate || cp > 0x10FFFF) {
    (*string)++;
    return kReplacement;
  }
  *string += bytes;
  return cp;
}

}  // namespace detail

inline std::string sanitizeFilename(const std::string& name, const size_t maxBytes) {
  std::string result;
  result.reserve(name.size() < maxBytes ? name.size() : maxBytes);
  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());
  const auto* rangeEnd = text + name.size();
  while (text < rangeEnd && (*text == ' ' || *text == '.')) text++;
  while (text < rangeEnd) {
    const auto* cpStart = text;
    const uint32_t cp = detail::utf8NextCodepoint(&text);
    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }
  const size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
  }
  return result.empty() ? "book" : result;
}

}  // namespace StringUtils
