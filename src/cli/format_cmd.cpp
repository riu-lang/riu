// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "format_cmd.h"

#include "tools/format/printer.h"
#include "tools/formatter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <toml.hpp>

namespace yux::cli {

int runFormatCommand(const FormatCmdOptions& opts) {
    std::string source;
    std::string filePath;

    if (opts.fromStdin) {
        std::stringstream buffer;
        buffer << std::cin.rdbuf();
        source = buffer.str();
    } else {
        if (opts.file.empty()) {
            std::cerr << "Error: No input file specified" << std::endl;
            return 1;
        }
        if (!std::filesystem::exists(opts.file)) {
            std::cerr << "Error: Input file not found: " << opts.file << std::endl;
            return 1;
        }

        std::ifstream inFile(opts.file);
        if (!inFile) {
            std::cerr << "Error: Cannot open file: " << opts.file << std::endl;
            return 1;
        }

        std::stringstream buffer;
        buffer << inFile.rdbuf();
        source = buffer.str();
        inFile.close();
        filePath = opts.file;
    }

    yux::FormatConfig config;

    // 优先使用命令行参数
    if (opts.lineWidth > 0) {
        config.lineWidth = static_cast<size_t>(opts.lineWidth);
    } else {
        namespace fs = std::filesystem;
        fs::path searchDir;
        if (!filePath.empty()) {
            searchDir = fs::path(filePath).parent_path();
        } else {
            searchDir = fs::current_path();
        }

        // 向上查找 yux.toml；用 prev 比较防止根目录 parent_path() 等于自身
        // 时陷入死循环（Windows `C:\` 的 parent_path 在某些实现下仍是 `C:\`）
        while (!searchDir.empty()) {
            fs::path tomlPath = searchDir / "yux.toml";
            if (fs::exists(tomlPath)) {
                try {
                    auto data = toml::parse(tomlPath.string());
                    if (data.contains("fmt")) {
                        const auto& fmt = data.at("fmt");
                        if (fmt.is_table()) {
                            if (fmt.contains("line_width") && fmt.at("line_width").is_integer()) {
                                config.lineWidth = static_cast<size_t>(fmt.at("line_width").as_integer());
                            }
                        }
                    }
                    break;
                } catch (const std::exception& e) {
                    // 解析失败，使用默认配置
                }
            }
            fs::path parent = searchDir.parent_path();
            if (parent.empty() || parent == searchDir) break;
            searchDir = parent;
        }
    }

    try {
        std::string formatted = yux::format::formatAst(source, config);

        if (opts.inPlace && !filePath.empty()) {
            std::ofstream outFile(filePath);
            if (!outFile) {
                std::cerr << "Error: Cannot write to file: " << filePath << std::endl;
                return 1;
            }
            outFile << formatted;
            outFile.close();
            std::cout << "Formatted: " << filePath << std::endl;
        } else {
            std::cout << formatted;
        }
    } catch (const std::exception& e) {
        std::cerr << "Format error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

} // namespace yux::cli
