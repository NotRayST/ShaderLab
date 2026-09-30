#pragma once
#include <reshade.hpp>
#include <vector>
#include <cstdint>
#include <cstring>
#include "ICommand.hpp"

struct DynamicPayload {
    reshade::api::format format = reshade::api::format::unknown;
    uint32_t rows = 0, cols = 0, element_count = 0;
    std::vector<uint8_t> bytes;

    size_t size_in_bytes() const noexcept { return bytes.size(); }

    bool operator==(const DynamicPayload &o) const noexcept {
        return format == o.format && rows == o.rows && cols == o.cols &&
               element_count == o.element_count && bytes == o.bytes;
    }
};
