// pk_unpack — Extract a packed LEGO Universe client to a directory tree.
//
// Usage:
//   pk_unpack <client_dir> <output_dir> [manifest]
//
// <client_dir>  Root of the packed client (must contain res/pack/).
// <output_dir>  Destination directory (created if needed). Files are written
//               preserving their original relative paths.
// [manifest]    Optional path to a manifest file (trunk.txt / hotfix.txt).
//               If omitted, searches <client_dir>/versions/trunk.txt and
//               <client_dir>/client/versions/trunk.txt automatically.
//               Without a manifest, files are extracted as entry_<crc>.bin.
//
// The manifest maps filenames to CRCs so files can be extracted with their
// correct paths. The CRC algorithm is CRC-32/ISO-HDLC (poly 0xEDB88320)
// applied to the lowercase backslash-normalised path.
//
// Files not referenced by the manifest (e.g. audio assets) are still
// extracted as crc_<hex>.bin in an _unknown/ subdirectory so no data is lost.

#include "netdevil/archive/vfs/vfs.h"
#include "netdevil/archive/pk/pk_reader.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static void print_help() {
    std::fprintf(stderr,
        "pk_unpack — Extract a packed LEGO Universe client to disk\n"
        "\n"
        "Usage: pk_unpack <client_dir> <output_dir> [manifest]\n"
        "\n"
        "  client_dir   Packed client root (must contain res/pack/)\n"
        "  output_dir   Destination directory (created if needed)\n"
        "  manifest     Optional manifest file (trunk.txt / hotfix.txt).\n"
        "               Searched automatically in <client_dir>/versions/ if omitted.\n"
        "\n"
        "Without a manifest, files are extracted as crc_<hex>.bin.\n"
        "Files matching the manifest are extracted with their original paths.\n");
}

// CRC-32/ISO-HDLC matching the LU pack file CRC algorithm.
static uint32_t pack_crc(const std::string& normalized) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char c : normalized) {
        crc ^= c;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFF;
}

static std::string normalize_path(const std::string& path) {
    std::string r = path;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
    return r;
}

// Parse a manifest file and return crc → relative path map.
// Manifest format: [files] section, one entry per line:
//   path,Bsize,Bchecksum,Asize,Achecksum,line_checksum
static std::unordered_map<uint32_t, std::string> load_manifest(const fs::path& manifest_path) {
    std::unordered_map<uint32_t, std::string> result;

    std::ifstream f(manifest_path);
    if (!f) return result;

    bool in_files = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[files]") { in_files = true; continue; }
        if (!line.empty() && line.front() == '[') { in_files = false; continue; }
        if (!in_files || line.empty()) continue;

        auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        std::string file_path = line.substr(0, comma);

        // Normalise: lowercase, backslash, strip leading "client/res/" or "client\"
        std::string norm = normalize_path(file_path);
        std::replace(norm.begin(), norm.end(), '/', '\\');
        // Ensure path starts with client\res\ for CRC matching
        if (norm.substr(0, 11) != "client\\res\\")
            norm = "client\\res\\" + norm;

        uint32_t crc = pack_crc(norm);
        // Store the forward-slash path without the client\res\ prefix for output
        std::string out_path = normalize_path(file_path);
        // Strip "client/res/" or "client/" prefix if present
        if (out_path.substr(0, 11) == "client/res/") out_path = out_path.substr(11);
        else if (out_path.substr(0, 7) == "client/") out_path = out_path.substr(7);

        result[crc] = out_path;
    }
    return result;
}

static bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    return out.good();
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_help();
        return 1;
    }

    std::string arg1 = argv[1];
    if (arg1 == "-h" || arg1 == "--help") { print_help(); return 0; }

    fs::path client_dir(argv[1]);
    fs::path output_dir(argv[2]);
    fs::path manifest_path = argc >= 4 ? fs::path(argv[3]) : fs::path{};

    // Validate: must have res/pack/
    fs::path pack_dir = client_dir / "res" / "pack";
    if (!fs::exists(pack_dir)) {
        // Try client/ subdirectory (0.179.12 layout)
        pack_dir = client_dir / "client" / "res" / "pack";
        if (!fs::exists(pack_dir)) {
            std::fprintf(stderr, "Error: no res/pack/ directory found in '%s'\n",
                client_dir.string().c_str());
            return 1;
        }
        client_dir = client_dir / "client";
    }

    // Find manifest if not specified
    if (manifest_path.empty()) {
        for (auto& candidate : {
            client_dir / "versions" / "trunk.txt",
            client_dir / "versions" / "hotfix.txt",
        }) {
            if (fs::exists(candidate)) { manifest_path = candidate; break; }
        }
    }

    // Load manifest (may be empty if no manifest found)
    std::unordered_map<uint32_t, std::string> crc_to_path;
    if (!manifest_path.empty() && fs::exists(manifest_path)) {
        crc_to_path = load_manifest(manifest_path);
        std::fprintf(stderr, "Manifest: %s (%zu file entries)\n",
            manifest_path.string().c_str(), crc_to_path.size());
    } else {
        std::fprintf(stderr, "No manifest found — files will be named by CRC\n");
    }

    // Load all .pk archives
    std::vector<fs::path> pk_files;
    for (auto& e : fs::directory_iterator(pack_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".pk")
            pk_files.push_back(e.path());
    }
    std::sort(pk_files.begin(), pk_files.end());

    if (pk_files.empty()) {
        std::fprintf(stderr, "Error: no .pk files in %s\n", pack_dir.string().c_str());
        return 1;
    }
    std::fprintf(stderr, "Found %zu .pk archives\n", pk_files.size());

    size_t extracted = 0, named = 0, unnamed = 0, failed = 0;

    for (auto& pk_path : pk_files) {
        std::ifstream in(pk_path, std::ios::binary | std::ios::ate);
        if (!in) { std::fprintf(stderr, "  skip: cannot open %s\n", pk_path.filename().string().c_str()); continue; }
        auto sz = in.tellg(); in.seekg(0);
        std::vector<uint8_t> pk_data(static_cast<size_t>(sz));
        in.read(reinterpret_cast<char*>(pk_data.data()), sz);

        lu::assets::PkArchive pk(pk_data);
        std::fprintf(stderr, "  %s: %zu entries\n", pk_path.filename().string().c_str(), pk.entry_count());

        pk.for_each([&](size_t, const lu::assets::PackIndexEntry& entry) {
            fs::path out_path;
            auto it = crc_to_path.find(entry.crc);
            if (it != crc_to_path.end()) {
                out_path = output_dir / it->second;
                ++named;
            } else {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "%08x", entry.crc);
                out_path = output_dir / "_unknown" / (std::string("crc_") + hex + ".bin");
                ++unnamed;
            }

            // Skip if already extracted (same file may appear in multiple packs)
            if (fs::exists(out_path)) { ++extracted; return; }

            try {
                auto data = pk.extract(entry);
                if (write_file(out_path, data)) {
                    ++extracted;
                } else {
                    std::fprintf(stderr, "    write failed: %s\n", out_path.string().c_str());
                    ++failed;
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "    extract error crc=%08x: %s\n", entry.crc, e.what());
                ++failed;
            }
        });
    }

    std::fprintf(stderr,
        "\nDone: %zu extracted (%zu named, %zu unnamed), %zu failed\n"
        "Output: %s\n",
        extracted, named, unnamed, failed, output_dir.string().c_str());

    return failed > 0 ? 1 : 0;
}
