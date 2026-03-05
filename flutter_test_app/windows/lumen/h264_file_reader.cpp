#include "h264_file_reader.h"

#include <fstream>

namespace lumen {

std::vector<NalUnit> FindNalUnits(const uint8_t* data, size_t size) {
    struct StartCode {
        size_t offset;
        uint8_t size;
    };
    std::vector<StartCode> start_codes;

    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                if (i > 0 && data[i - 1] == 0x00) {
                    if (!start_codes.empty() && start_codes.back().offset == i - 1) {
                        start_codes.back().offset = i - 1;
                        start_codes.back().size = 4;
                    } else {
                        start_codes.push_back({i - 1, 4});
                    }
                } else {
                    start_codes.push_back({i, 3});
                }
                i += 2;
            } else if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                start_codes.push_back({i, 4});
                i += 3;
            }
        }
    }

    std::vector<NalUnit> nals;
    for (size_t i = 0; i < start_codes.size(); ++i) {
        size_t end = (i + 1 < start_codes.size()) ? start_codes[i + 1].offset : size;
        nals.push_back({start_codes[i].offset,
                        end - start_codes[i].offset,
                        start_codes[i].size});
    }

    return nals;
}

std::vector<std::pair<size_t, size_t>> GroupAccessUnits(
    const uint8_t* data, const std::vector<NalUnit>& nals) {

    std::vector<size_t> au_starts;
    bool last_was_vcl = false;

    for (size_t i = 0; i < nals.size(); ++i) {
        uint8_t nal_type = data[nals[i].offset + nals[i].start_code_size] & 0x1F;
        bool is_vcl = (nal_type >= 1 && nal_type <= 5);

        if (nal_type == 7) {
            au_starts.push_back(i);
            last_was_vcl = false;
        } else if (is_vcl) {
            if (!last_was_vcl) {
                if (au_starts.empty()) {
                    au_starts.push_back(i);
                } else {
                    bool current_au_has_vcl = false;
                    for (size_t j = au_starts.back(); j < i; ++j) {
                        uint8_t jt = data[nals[j].offset + nals[j].start_code_size] & 0x1F;
                        if (jt >= 1 && jt <= 5) {
                            current_au_has_vcl = true;
                            break;
                        }
                    }
                    if (current_au_has_vcl) {
                        au_starts.push_back(i);
                    }
                }
            }
            last_was_vcl = true;
        } else {
            if (au_starts.empty()) {
                au_starts.push_back(i);
            }
            last_was_vcl = false;
        }
    }

    std::vector<std::pair<size_t, size_t>> aus;
    for (size_t i = 0; i < au_starts.size(); ++i) {
        size_t start = au_starts[i];
        size_t end = (i + 1 < au_starts.size()) ? au_starts[i + 1] : nals.size();
        aus.emplace_back(start, end);
    }

    return aus;
}

H264Bitstream LoadH264File(const std::string& path) {
    H264Bitstream result;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return result;
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    result.data.resize(file_size);
    file.read(reinterpret_cast<char*>(result.data.data()), file_size);
    file.close();

    result.nals = FindNalUnits(result.data.data(), result.data.size());
    result.access_units = GroupAccessUnits(result.data.data(), result.nals);

    return result;
}

}  // namespace lumen
