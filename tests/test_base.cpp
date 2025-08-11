// Copyright (c) 2026. Yin-Jinlong@github

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <array>
#include <vector>

namespace fs = std::filesystem;

static std::string exec(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

static std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static fs::path getTestCasesDir() {
    return fs::current_path() / "tests" / "cases";
}

static fs::path getYuxCompiler() {
#ifdef _WIN32
    return "yux";
#else
    return "./yux";
#endif
}

struct TestCase {
    fs::path yuxFile;
    fs::path expectedFile;
    std::string name;
};

class YuxCompilerTest : public ::testing::TestWithParam<TestCase> {};

TEST_P(YuxCompilerTest, CompileAndCompareOutput) {
    const auto& tc = GetParam();
    
    fs::path yuxFile = tc.yuxFile;
    fs::path expectedFile = tc.expectedFile;
    
    ASSERT_TRUE(fs::exists(yuxFile)) << "Yux file not found: " << yuxFile;
    ASSERT_TRUE(fs::exists(expectedFile)) << "Expected file not found: " << expectedFile;
    
    fs::path workDir = yuxFile.parent_path();
    std::string stem = yuxFile.stem().string();
    
#ifdef _WIN32
    fs::path exeFile = workDir / (stem + ".exe");
    fs::path objFile = workDir / (stem + ".obj");
#else
    fs::path exeFile = workDir / stem;
    fs::path objFile = workDir / (stem + ".o");
#endif
    
    if (fs::exists(exeFile)) fs::remove(exeFile);
    if (fs::exists(objFile)) fs::remove(objFile);
    
    fs::path originalDir = fs::current_path();
    fs::current_path(workDir);
    
    std::string compileCmd = getYuxCompiler().string() + " \"" + yuxFile.filename().string() + "\"";
    int compileResult = std::system(compileCmd.c_str());
    
    fs::current_path(originalDir);
    
    ASSERT_EQ(compileResult, 0) << "Compilation failed for: " << yuxFile;
    
    ASSERT_TRUE(fs::exists(exeFile)) << "Executable not generated: " << exeFile;
    
    std::string runCmd = "\"" + exeFile.string() + "\"";
    std::string actualOutput = exec(runCmd);
    
    std::string expectedOutput = readFile(expectedFile);
    
    EXPECT_EQ(actualOutput, expectedOutput) 
        << "Output mismatch for " << yuxFile << "\n"
        << "Expected:\n" << expectedOutput << "\n"
        << "Actual:\n" << actualOutput;
    
    if (fs::exists(exeFile)) fs::remove(exeFile);
    if (fs::exists(objFile)) fs::remove(objFile);
}

std::vector<TestCase> discoverTestCases() {
    std::vector<TestCase> cases;
    fs::path casesDir = getTestCasesDir();
    
    if (!fs::exists(casesDir)) {
        return cases;
    }
    
    for (const auto& entry : fs::directory_iterator(casesDir)) {
        if (entry.path().extension() == ".yux") {
            fs::path yuxFile = entry.path();
            fs::path expectedFile = yuxFile;
            expectedFile.replace_extension(".expected");
            
            if (fs::exists(expectedFile)) {
                cases.push_back({
                    yuxFile,
                    expectedFile,
                    yuxFile.stem().string()
                });
            }
        }
    }
    
    return cases;
}

INSTANTIATE_TEST_SUITE_P(
    YuxTests,
    YuxCompilerTest,
    ::testing::ValuesIn(discoverTestCases()),
    [](const ::testing::TestParamInfo<TestCase>& info) {
        return info.param.name;
    }
);
