#include "netdevil/archive/pk/pk_reader.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: pk_extractor <file.pk> [output_dir]\n";
        return 1;
    }

    const char* input_path = argv[1];
    std::string output_dir = argc > 2 ? argv[2] : ".";

    // Read input file
    std::ifstream file(input_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Error: cannot open " << input_path << "\n";
        return 1;
    }

    auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    try {
        lu::assets::PkArchive pk(data);
        std::cout << "PK archive: " << pk.entry_count() << " entries\n";

        pk.for_each([&](size_t idx, const lu::assets::PackIndexEntry& entry) {
            std::cout << "  [" << idx << "] crc=0x" << std::hex << entry.crc
                      << " size=" << std::dec << entry.uncompressed_size
                      << " compressed=" << (entry.is_compressed ? "yes" : "no")
                      << " offset=" << entry.data_offset << "\n";

            // Extract to output directory
            try {
                auto extracted = pk.extract(entry);
                std::string out_file = output_dir + "/entry_" + std::to_string(idx) + ".bin";
                std::ofstream out(out_file, std::ios::binary);
                out.write(reinterpret_cast<const char*>(extracted.data()), extracted.size());
                std::cout << "    -> " << out_file << " (" << extracted.size() << " bytes)\n";
            } catch (const std::exception& e) {
                std::cerr << "    ERROR: " << e.what() << "\n";
            }
        });
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
