#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <span>
#include <regex>

// ----------------------------------------------------------------            
// 基本設定と構造体
// ----------------------------------------------------------------
struct TfmEntry {
    size_t frame;
    char type;       // 'p', 'c', 'h'
    bool is_combed;  // '+' if true, '-' if false
    int mic;         // mic value
};

enum class CyclePhase {
    PPCCC = 0, // p at remainder 0, 1
    CPPCC = 1, // p at remainder 1, 2
    CCPPC = 2, // p at remainder 2, 3
    CCCPP = 3, // p at remainder 3, 4
    PCCCP = 4, // p at remainder 4, 0
    Unknown = 5
};

static const std::array<std::string, 6> PATTERN_NAMES = {
    "ppccc", "cppcc", "ccppc", "cccpp", "pcccp", "c"
};

// ----------------------------------------------------------------            
// 判定・探索ロジック
// ----------------------------------------------------------------

// ノイズ判定 (mic 80以上, h判定, 縞検出あり) - これらはワイルドカードとして扱う
static bool is_noise(const TfmEntry& e) {
    return (e.mic >= 80 || e.type == 'h' || e.is_combed);
}

// そのフレームが指定フェーズにおいて 'p' であるべき位置か判定
static bool is_p_position(size_t frame, CyclePhase phase) {
    if (phase == CyclePhase::Unknown) return false;
    int r = static_cast<int>(frame % 5);
    int p1 = static_cast<int>(phase);
    int p2 = (p1 + 1) % 5;
    return (r == p1 || r == p2);
}

// 特定フェーズに対する不一致（エラー）数をカウント
static int count_errors(std::span<const TfmEntry> window, CyclePhase phase) {
    int errors = 0;
    for (const auto& e : window) {
        if (is_noise(e)) continue; // ワイルドカード
        bool should_be_p = is_p_position(e.frame, phase);
        if (should_be_p && e.type == 'c') errors++;
        else if (!should_be_p && e.type == 'p') errors++;
    }
    return errors;
}

// 範囲内の支配的なフェーズを計算
static CyclePhase detect_dominant_phase(std::span<const TfmEntry> window, CyclePhase current) {
    if (window.empty()) return current;

    std::array<int, 5> counts = { 0, 0, 0, 0, 0 };
    int valid_p_count = 0;

    for (const auto& e : window) {
        if (e.type == 'p' && !is_noise(e)) {
            counts[e.frame % 5]++;
            valid_p_count++;
        }
    }

    // 情報不足：100フレーム中 8個以上の有効なpがない場合は判定を更新しない
    if (valid_p_count < 8) return current;

    int max_sum = -1;
    int best_p1 = -1;
    for (int i = 0; i < 5; ++i) {
        int current_sum = counts[static_cast<size_t>(i)] + counts[static_cast<size_t>((i + 1) % 5)];
        if (current_sum > max_sum) {
            max_sum = current_sum;
            best_p1 = i;
        }
    }

    CyclePhase candidate = static_cast<CyclePhase>(best_p1);
    double best_ratio = static_cast<double>(max_sum) / static_cast<double>(valid_p_count);

    // 支配率が著しく低い(60%未満)場合は無理に更新しない
    if (best_ratio < 0.6) return current;

    // 強力な慣性ロジック：現在のフェーズがまだ 70% 以上の整合性を持っているなら維持
    if (current != CyclePhase::Unknown) {
        int current_errors = count_errors(window, current);
        double current_accuracy = 1.0 - (static_cast<double>(current_errors) / static_cast<double>(valid_p_count));
        if (current_accuracy >= 0.7) return current;
    }

    return candidate;
}

// 2つのフェーズの境界（不連続点）をエラー最小化で特定
static size_t find_boundary(std::span<const TfmEntry> window, CyclePhase old_p, CyclePhase new_p, size_t last_boundary) {
    size_t best_t = 0;
    int64_t min_error = 1000000;

    for (size_t t = 0; t <= window.size(); ++t) {
        int64_t errors = 0;
        for (size_t i = 0; i < window.size(); ++i) {
            if (is_noise(window[i])) continue;

            CyclePhase expected = (i < t) ? old_p : new_p;
            bool should_be_p = is_p_position(window[i].frame, expected);

            if (should_be_p && window[i].type == 'c') errors++;
            else if (!should_be_p && window[i].type == 'p') errors++;
        }

        // 同点なら後ろ（変更を遅らせる方向）を優先して安定させる
        if (errors < min_error) {
            min_error = errors;
            best_t = t;
        }
    }
    size_t safe_idx = (best_t < window.size()) ? best_t : window.size() - 1;
    // 前回の境界より後ろであることを保証し、逆転を防ぐ
    return std::max(window[safe_idx].frame, last_boundary + 1);
}

// ----------------------------------------------------------------            
// メイン処理
// ----------------------------------------------------------------
static void print_usage() {
    std::cout << "--------------------------------------" << std::endl;
    std::cout << "generate_tfmovr4anime v1.0.0 by Ikotas" << std::endl;
    std::cout << "--------------------------------------" << std::endl;
    std::cout << "Usage: generate_tfmovr4anime.exe [options] TFM_output_file(*.tfm)" << std::endl << std::endl;
    std::cout << "The output of TFM is as follows:" << std::endl;
    std::cout << "TFM(mode=0,pp=1,slow=2,micmatching=0,output=\"x:\\path\\filename.tfm\")" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string input_path = argv[argc - 1];
    if (input_path.find(".tfm") == std::string::npos) input_path += ".tfm";

    std::ifstream ifs(input_path);
    if (!ifs) {
        std::cerr << "Error: " << input_path << " not found." << std::endl;
        std::cerr << "Make sure the output file for TFM is in the same folder." << std::endl << std::endl;
        std::cerr << "The output of TFM is as follows:" << std::endl;
        std::cerr << "TFM(mode=0,pp=1,slow=2,micmatching=0,output=\"x:\\path\\filename.tfm\")" << std::endl;
        return 1;
    }

    std::vector<TfmEntry> entries;
    std::string line;
    std::regex re(R"((\d+)\s+([cph])\s+([\+\-])\s+\[(\d+)\])");
    std::smatch m;

    while (std::getline(ifs, line)) {
        if (std::regex_search(line, m, re)) {
            entries.push_back({
                static_cast<size_t>(std::stoull(m[1].str())),
                m[2].str()[0],
                m[3].str() == "+",
                std::stoi(m[4].str())
                });
        }
    }

    if (entries.empty()) return 0;

    std::vector<std::pair<size_t, CyclePhase>> results;
    CyclePhase current_phase = CyclePhase::Unknown;
    results.push_back({ 0, CyclePhase::Unknown });

    const size_t window_size = 100;

    // 処理の安定と高速化のため5フレーム（1サイクル）刻みでスキャン
    for (size_t i = 0; i < entries.size(); i += 5) {
        size_t start = (i < window_size / 2) ? 0 : i - window_size / 2;
        size_t end = std::min(entries.size(), start + window_size);
        std::span<const TfmEntry> window(entries.data() + start, end - start);

        CyclePhase detected = detect_dominant_phase(window, current_phase);

        if (detected != CyclePhase::Unknown && detected != current_phase) {
            if (current_phase == CyclePhase::Unknown) {
                results.back().second = detected;
                current_phase = detected;
            }
            else {
                size_t boundary = find_boundary(window, current_phase, detected, results.back().first);
                results.push_back({ boundary, detected });
                current_phase = detected;
            }
        }
    }

    // 出力処理
    std::string out_path = input_path.substr(0, input_path.find_last_of('.')) + ".tfmovr";
    std::ofstream ofs(out_path);

    for (size_t i = 0; i < results.size(); ++i) {
        size_t start = results[i].first;
        size_t next_start = (i + 1 < results.size()) ? results[i + 1].first : 0;
        size_t end_val = (next_start > 0) ? next_start - 1 : 0;

        std::string pattern = (results[i].second == CyclePhase::Unknown) ? "c" : PATTERN_NAMES[static_cast<size_t>(results[i].second)];

        ofs << start << "," << end_val << " " << pattern << "\n";
    }

    std::cout << "Successfully generated: " << out_path << " (" << results.size() << " entries)" << std::endl;

    return 0;
}
