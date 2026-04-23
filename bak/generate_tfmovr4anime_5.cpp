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
    P01 = 0, P12 = 1, P23 = 2, P34 = 3, P40 = 4, Unknown = 5
};

// 開始フレームに基づいた相対的なパターン名を生成する (v1.2.0準拠)
static std::string generate_pattern_name(size_t start_frame, CyclePhase phase) {
    if (phase == CyclePhase::Unknown) return "c";
    std::string pattern = "ccccc";
    int p1 = static_cast<int>(phase);
    int p2 = (p1 + 1) % 5;
    for (int j = 0; j < 5; ++j) {
        int current_rem = static_cast<int>((start_frame + j) % 5);
        if (current_rem == p1 || current_rem == p2) pattern[j] = 'p';
    }
    return pattern;
}

// ----------------------------------------------------------------            
// 判定・探索ロジック
// ----------------------------------------------------------------

static bool is_noise(const TfmEntry& e) {
    return (e.mic >= 80 || e.type == 'h' || e.is_combed);
}

static bool is_p_position(size_t frame, CyclePhase phase) {
    if (phase == CyclePhase::Unknown) return false;
    int r = static_cast<int>(frame % 5);
    int p1 = static_cast<int>(phase);
    int p2 = (p1 + 1) % 5;
    return (r == p1 || r == p2);
}

// 慣性判定専用：現在のフェーズと「衝突」するpの数を数える
static double calculate_inertia_accuracy(std::span<const TfmEntry> window, CyclePhase phase) {
    int total_actual_p = 0;
    int conflicts = 0;
    for (const auto& e : window) {
        if (e.type == 'p' && !is_noise(e)) {
            total_actual_p++;
            // 本来 c であるべき場所に p が出ている場合を「衝突」とする
            if (!is_p_position(e.frame, phase)) conflicts++;
        }
    }
    if (total_actual_p == 0) return 1.0; // 動きがないなら矛盾なしと見なす
    return 1.0 - (static_cast<double>(conflicts) / total_actual_p);
}

static CyclePhase detect_dominant_phase(std::span<const TfmEntry> window, CyclePhase current) {
    if (window.empty()) return current;

    std::array<int, 5> counts = { 0, 0, 0, 0, 0 };
    int total_valid_p = 0;
    for (const auto& e : window) {
        if (e.type == 'p' && !is_noise(e)) {
            counts[e.frame % 5]++;
            total_valid_p++;
        }
    }

    // 動きが少なすぎる場合は判断を保留
    if (total_valid_p < 6) return current;

    long long max_sum = -1;
    int best_phase_idx = -1;
    for (int i = 0; i < 5; ++i) {
        long long sum = static_cast<long long>(counts[i]) + counts[(i + 1) % 5];
        if (sum > max_sum) {
            max_sum = sum;
            best_phase_idx = i;
        }
    }

    double ratio = static_cast<double>(max_sum) / total_valid_p;

    // 動きの密度に応じた閾値 (v1.2.0)
    double required_ratio = (total_valid_p > 25) ? 0.7 : 0.9;
    if (ratio < required_ratio) return current;

    CyclePhase candidate = static_cast<CyclePhase>(best_phase_idx);

    // 【修正】衝突ベースの慣性判定
    // 現在のフェーズと矛盾する p が 20% 以下（精度80%以上）なら、
    // 静止画による欠損があってもサイクルを維持する
    if (current != CyclePhase::Unknown) {
        double current_acc = calculate_inertia_accuracy(window, current);
        if (current_acc >= 0.8) return current;
    }

    return candidate;
}

static size_t find_boundary(std::span<const TfmEntry> window, CyclePhase old_p, CyclePhase new_p, size_t last_boundary) {
    size_t best_t = 0;
    int64_t min_error = 1000000;

    for (size_t t = 0; t <= window.size(); ++t) {
        int64_t errors = 0;
        for (size_t i = 0; i < window.size(); ++i) {
            if (is_noise(window[i])) continue;
            CyclePhase expected = (i < t) ? old_p : new_p;
            if (is_p_position(window[i].frame, expected)) {
                if (window[i].type == 'c') errors++;
            }
            else {
                if (window[i].type == 'p') errors++;
            }
        }
        // 同値なら後ろ（現在のスキャン位置に近い方）を優先
        if (errors <= min_error) {
            min_error = errors;
            best_t = t;
        }
    }
    size_t safe_idx = (best_t < window.size()) ? best_t : window.size() - 1;
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
                static_cast<size_t>(std::stoull(m.str(1))),
                m.str(2)[0],
                m.str(3) == "+",
                std::stoi(m.str(4))
                });
        }
    }

    if (entries.empty()) return 0;

    std::vector<std::pair<size_t, CyclePhase>> results;
    CyclePhase current_phase = CyclePhase::Unknown;
    size_t last_start_frame = 0;

    const size_t window_size = 100;
    const size_t MIN_SEGMENT_LENGTH = 150;

    for (size_t i = 0; i < entries.size(); i += 5) {
        size_t start = (i < window_size / 2) ? 0 : i - window_size / 2;
        size_t end = std::min(entries.size(), start + window_size);
        std::span<const TfmEntry> window(entries.data() + start, end - start);

        CyclePhase detected = detect_dominant_phase(window, current_phase);

        if (detected != CyclePhase::Unknown && detected != current_phase) {
            if (current_phase == CyclePhase::Unknown) {
                results.push_back({ entries[i].frame, detected });
                current_phase = detected;
                last_start_frame = entries[i].frame;
            }
            else if (entries[i].frame - last_start_frame >= MIN_SEGMENT_LENGTH) {
                size_t boundary = find_boundary(window, current_phase, detected, last_start_frame);
                if (boundary - last_start_frame >= MIN_SEGMENT_LENGTH) {
                    results.push_back({ boundary, detected });
                    current_phase = detected;
                    last_start_frame = boundary;
                }
            }
        }
    }

    std::string out_path = input_path.substr(0, input_path.find_last_of('.')) + ".tfmovr";
    std::ofstream ofs(out_path);

    for (size_t i = 0; i < results.size(); ++i) {
        size_t start = results[i].first;
        size_t next_start = (i + 1 < results.size()) ? results[i + 1].first : 0;
        size_t end_val = (next_start > 0) ? next_start - 1 : 0;

        std::string pattern = generate_pattern_name(start, results[i].second);
        ofs << start << "," << end_val << " " << pattern << "\n";
    }

    std::cout << "Successfully generated: " << out_path << " (" << results.size() << " entries)" << std::endl;
    return 0;
}
