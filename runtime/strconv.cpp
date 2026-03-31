#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>

extern "C" auto cyan_strconv_write_f64(char* out, std::int64_t capacity,
                                       double value) -> std::int64_t {
    if (out == nullptr || capacity <= 0) {
        return -1;
    }

    auto* const begin = out;
    auto* const end = out + static_cast<std::size_t>(capacity - 1);
    const auto [ptr, ec] =
        std::to_chars(begin, end, value, std::chars_format::general);
    if (ec != std::errc()) {
        out[0] = '\0';
        return -1;
    }

    *ptr = '\0';
    return static_cast<std::int64_t>(ptr - begin);
}
