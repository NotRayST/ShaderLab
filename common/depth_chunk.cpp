#include "depth_chunk.h"
#include "../third_party/miniz/miniz.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace depth_chunk {

static inline uint32_t read_be32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

static inline void write_be32(uint8_t *p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8)  & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

static const uint32_t s_png_crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

uint32_t calc_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = s_png_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

bool extract_sldp(
    const uint8_t *png_data,
    size_t size,
    DepthMapHeader &out_header,
    std::vector<float> &out_floats,
    std::string &out_error)
{
    out_floats.clear();
    std::memset(&out_header, 0, sizeof(out_header));

    static const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (!png_data || size < 32) {
        out_error = "File is too small or null";
        return false;
    }
    if (std::memcmp(png_data, png_sig, 8) != 0) {
        out_error = "Invalid PNG signature";
        return false;
    }

    size_t offset = 8;
    bool found_sldp = false;
    const uint8_t *chunk_payload = nullptr;
    uint32_t chunk_len = 0;

    while (offset + 12 <= size) {
        uint32_t len = read_be32(png_data + offset);
        const uint8_t *type = png_data + offset + 4;

        if (offset + 12 + len > size) {
            out_error = "Corrupt PNG: chunk length exceeds file size";
            return false;
        }

        uint32_t stored_crc = read_be32(png_data + offset + 8 + len);
        uint32_t actual_crc = calc_crc32(type, len + 4);
        if (stored_crc != actual_crc) {
            out_error = "Corrupt PNG: CRC32 mismatch on chunk";
            return false;
        }

        if (std::memcmp(type, "slDp", 4) == 0 && len >= sizeof(DepthMapHeader)) {
            const uint8_t *payload = png_data + offset + 8;
            uint32_t magic = 0;
            std::memcpy(&magic, payload, sizeof(magic));
            if (magic == kDepthMagicSLD1 || magic == 0x50444C53) {
                found_sldp = true;
                chunk_payload = payload;
                chunk_len = len;
                break;
            }
        }

        if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }

        offset += 12 + len;
    }

    if (!found_sldp || !chunk_payload) {
        out_error = "slDp chunk not found";
        return false;
    }

    if (chunk_len < sizeof(DepthMapHeader)) {
        out_error = "slDp chunk is smaller than DepthMapHeader";
        return false;
    }

    std::memcpy(&out_header, chunk_payload, sizeof(DepthMapHeader));

    if (out_header.magic != kDepthMagicSLD1 && out_header.magic != 0x50444C53) {
        out_error = "Invalid slDp magic (expected 'SLD1' or 'SLDP')";
        return false;
    }

    if (out_header.version != kDepthVersion1) {
        out_error = "Unsupported slDp version";
        return false;
    }

    uint32_t w = out_header.width;
    uint32_t h = out_header.height;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        out_error = "Invalid depth dimensions (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        return false;
    }

    size_t expected_raw_size = (out_header.encoding == 1)
        ? (static_cast<size_t>(w) * h * sizeof(uint16_t))
        : (static_cast<size_t>(w) * h * sizeof(float));

    if (out_header.raw_byte_size != expected_raw_size) {
        out_error = "raw_byte_size header mismatch";
        return false;
    }

    const uint8_t *comp_data = chunk_payload + sizeof(DepthMapHeader);
    size_t comp_size = chunk_len - sizeof(DepthMapHeader);

    out_floats.resize(w * h);
    mz_ulong uncomp_len = static_cast<mz_ulong>(expected_raw_size);

    if (out_header.encoding == 1) {
        std::vector<uint16_t> u16_data(w * h);
        int status = mz_uncompress(
            reinterpret_cast<unsigned char *>(u16_data.data()),
            &uncomp_len,
            comp_data,
            static_cast<mz_ulong>(comp_size)
        );
        if (status != MZ_OK || uncomp_len != expected_raw_size) {
            out_floats.clear();
            out_error = "Decompression failed (error " + std::to_string(status) + ")";
            return false;
        }
        // horizontal delta reconstruction
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t row_start = y * w;
            uint16_t running = 0;
            for (uint32_t x = 0; x < w; ++x) {
                running += u16_data[row_start + x];
                out_floats[row_start + x] = running / 65535.0f;
            }
        }
    } else {
        int status = mz_uncompress(
            reinterpret_cast<unsigned char *>(out_floats.data()),
            &uncomp_len,
            comp_data,
            static_cast<mz_ulong>(comp_size)
        );
        if (status != MZ_OK || uncomp_len != expected_raw_size) {
            out_floats.clear();
            out_error = "Decompression failed (error " + std::to_string(status) + ")";
            return false;
        }
    }

    // sanitize floats (clamp to [0, 1], replace nans with 1.0)
    for (size_t i = 0; i < out_floats.size(); ++i) {
        float v = out_floats[i];
        if (std::isnan(v) || std::isinf(v)) {
            out_floats[i] = 1.0f;
        } else {
            out_floats[i] = std::clamp(v, 0.0f, 1.0f);
        }
    }

    return true;
}

bool splice_sldp_bytes(
    const uint8_t *in_png,
    size_t in_size,
    const DepthMapHeader &header,
    const float *depth_floats,
    size_t count,
    std::vector<uint8_t> &out_png,
    std::string &out_error)
{
    out_png.clear();

    if (!in_png || in_size < 32 || !depth_floats || count == 0) {
        out_error = "Invalid parameters for splice_sldp_bytes";
        return false;
    }

    if (count != static_cast<size_t>(header.width) * header.height) {
        out_error = "Float count does not match header dimensions";
        return false;
    }

    // deflate depth payload
    size_t raw_size = (header.encoding == 1) ? (count * sizeof(uint16_t)) : (count * sizeof(float));
    std::vector<uint16_t> u16_buf;
    const void *src_ptr = depth_floats;

    if (header.encoding == 1) {
        uint32_t width = header.width;
        uint32_t height = header.height;
        u16_buf.resize(count);
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t row_start = y * width;
            uint16_t prev = 0;
            for (uint32_t x = 0; x < width; ++x) {
                float v = std::clamp(depth_floats[row_start + x], 0.0f, 1.0f);
                uint16_t curr = static_cast<uint16_t>(v * 65535.0f + 0.5f);
                u16_buf[row_start + x] = curr - prev;
                prev = curr;
            }
        }
        src_ptr = u16_buf.data();
    }

    mz_ulong comp_bound = mz_compressBound(static_cast<mz_ulong>(raw_size));
    std::vector<uint8_t> comp_buf(comp_bound);
    mz_ulong comp_len = comp_bound;

    int status = mz_compress(
        comp_buf.data(),
        &comp_len,
        reinterpret_cast<const unsigned char *>(src_ptr),
        static_cast<mz_ulong>(raw_size)
    );

    if (status != MZ_OK || comp_len == 0) {
        out_error = "Compression failed (error " + std::to_string(status) + ")";
        return false;
    }

    comp_buf.resize(comp_len);

    DepthMapHeader final_header = header;
    final_header.magic = kDepthMagicSLD1;
    final_header.version = kDepthVersion1;
    final_header.encoding = header.encoding;
    final_header.raw_byte_size = static_cast<uint32_t>(raw_size);
    final_header.compressed_byte_size = static_cast<uint32_t>(comp_len);

    size_t iend_offset = in_size;
    for (size_t i = in_size - 12; i >= 8; --i) {
        if (std::memcmp(in_png + i + 4, "IEND", 4) == 0) {
            iend_offset = i;
            break;
        }
    }

    if (iend_offset >= in_size) {
        out_error = "Could not locate IEND marker in input PNG";
        return false;
    }

    uint32_t chunk_data_len = static_cast<uint32_t>(sizeof(DepthMapHeader) + comp_len);
    std::vector<uint8_t> sldp_chunk(12 + chunk_data_len);

    write_be32(sldp_chunk.data(), chunk_data_len);
    std::memcpy(sldp_chunk.data() + 4, "slDp", 4);
    std::memcpy(sldp_chunk.data() + 8, &final_header, sizeof(DepthMapHeader));
    std::memcpy(sldp_chunk.data() + 8 + sizeof(DepthMapHeader), comp_buf.data(), comp_len);
    uint32_t crc = calc_crc32(sldp_chunk.data() + 4, 4 + chunk_data_len);
    write_be32(sldp_chunk.data() + 8 + chunk_data_len, crc);

    //strip old chunk so we dont keep duplicating sldp metadata
    std::vector<uint8_t> clean_png;
    clean_png.reserve(in_size + sldp_chunk.size());
    clean_png.insert(clean_png.end(), in_png, in_png + 8);
    size_t cur_off = 8;
    while (cur_off + 12 <= in_size) {
        uint32_t c_len = read_be32(in_png + cur_off);
        const uint8_t *c_type = in_png + cur_off + 4;
        if (std::memcmp(c_type, "IEND", 4) == 0) {
            break;
        }
        if (std::memcmp(c_type, "slDp", 4) != 0) {
            clean_png.insert(clean_png.end(), in_png + cur_off, in_png + cur_off + 12 + c_len);
        }
        cur_off += 12 + c_len;
    }

    clean_png.insert(clean_png.end(), sldp_chunk.begin(), sldp_chunk.end());
    if (cur_off + 12 <= in_size) {
        clean_png.insert(clean_png.end(), in_png + cur_off, in_png + cur_off + 12);
    } else {
        static const uint8_t standard_iend[12] = { 0,0,0,0, 'I','E','N','D', 0xAE,0x42,0x60,0x82 };
        clean_png.insert(clean_png.end(), standard_iend, standard_iend + 12);
    }
    out_png = std::move(clean_png);

    return true;
}

bool inject_sldp(
    const std::string &png_path,
    const DepthMapHeader &header,
    const float *depth_floats,
    size_t count,
    std::string &out_error)
{
    std::ifstream in(png_path, std::ios::binary | std::ios::ate);
    if (!in) {
        out_error = "Cannot open input file: " + png_path;
        return false;
    }

    std::streamsize file_size = in.tellg();
    if (file_size < 32) {
        out_error = "File is too small to be a valid PNG: " + png_path;
        return false;
    }

    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> in_buffer(static_cast<size_t>(file_size));
    if (!in.read(reinterpret_cast<char *>(in_buffer.data()), file_size)) {
        out_error = "Failed to read file: " + png_path;
        return false;
    }
    in.close();

    std::vector<uint8_t> out_buffer;
    if (!splice_sldp_bytes(in_buffer.data(), in_buffer.size(), header, depth_floats, count, out_buffer, out_error)) {
        return false;
    }

    std::ofstream out(png_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        out_error = "Cannot open file for writing: " + png_path;
        return false;
    }

    if (!out.write(reinterpret_cast<const char *>(out_buffer.data()), out_buffer.size())) {
        out_error = "Failed to write modified PNG: " + png_path;
        return false;
    }
    out.close();

    return true;
}

} // namespace depth_chunk
