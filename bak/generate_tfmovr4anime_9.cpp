#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <array>
#include <algorithm>
#include <span>
#include <regex>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;

// ----------------------------------------------------------------            
// 基本設定と構造体 (generate_tfmovr.cpp と共通化)
// ----------------------------------------------------------------
const vector<string> VALID_PATTERNS = { "cccpp", "pcccp", "ppccc", "cppcc", "ccppc" };

struct TfmEntry {
    size_t frame;
    char type;       // 'p', 'c', 'h'
    bool is_combed;  // '+' if true, '-' if false
    int mic;         // mic value
};

enum class CyclePhase {
    P01 = 0, P12 = 1, P23 = 2, P34 = 3, P40 = 4, Unknown = 5
};

// 開始フレームに基づいた相対的なパターン名を生成する
static string generate_pattern_name(size_t start_frame, CyclePhase phase) {
    if (phase == CyclePhase::Unknown) return "c";
    string pattern = "ccccc";
    int p1 = static_cast<int>(phase);
    int p2 = (p1 + 1) % 5;
    for (int j = 0; j < 5; ++j) {
        int current_rem = static_cast<int>((start_frame + static_cast<size_t>(j)) % 5);
        if (current_rem == p1 || current_rem == p2) pattern[static_cast<size_t>(j)] = 'p';
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

static double calculate_inertia_accuracy(span<const TfmEntry> window, CyclePhase phase) {
    int64_t total_actual_p = 0;
    int64_t conflicts = 0;
    for (const auto& e : window) {
        if (e.type == 'p' && !is_noise(e)) {
            total_actual_p++;
            if (!is_p_position(e.frame, phase)) conflicts++;
        }
    }
    if (total_actual_p == 0) return 1.0;
    return 1.0 - (static_cast<double>(conflicts) / static_cast<double>(total_actual_p));
}

static CyclePhase detect_dominant_phase(span<const TfmEntry> window, CyclePhase current,
    int64_t threshold_min_p, double ratio_threshold, double consistency_acc) {
    if (window.empty()) return current;

    array<int64_t, 5> counts = { 0, 0, 0, 0, 0 };
    int64_t total_valid_p = 0;
    for (const auto& e : window) {
        if (e.type == 'p' && !is_noise(e)) {
            counts[static_cast<size_t>(e.frame % 5)]++;
            total_valid_p++;
        }
    }

    if (total_valid_p < threshold_min_p) return current;

    int64_t max_sum = -1;
    int best_phase_idx = -1;
    for (int i = 0; i < 5; ++i) {
        int64_t current_sum = counts[static_cast<size_t>(i)] + counts[static_cast<size_t>((i + 1) % 5)];
        if (current_sum > max_sum) {
            max_sum = current_sum;
            best_phase_idx = i;
        }
    }

    double ratio = static_cast<double>(max_sum) / static_cast<double>(total_valid_p);
    if (ratio < ratio_threshold) return current;

    CyclePhase candidate = static_cast<CyclePhase>(best_phase_idx);

    if (current != CyclePhase::Unknown) {
        double current_acc = calculate_inertia_accuracy(window, current);
        if (current_acc >= consistency_acc) return current;
    }

    return candidate;
}

static size_t find_boundary(span<const TfmEntry> window, CyclePhase old_p, CyclePhase new_p, size_t last_boundary) {
    size_t best_t = 0;
    int64_t min_error = 2000000000;

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
        if (errors < min_error) {
            min_error = errors;
            best_t = t;
        }
    }
    size_t safe_idx = (best_t < window.size()) ? best_t : window.size() - 1;
    return max(window[safe_idx].frame, last_boundary + 1);
}

// ----------------------------------------------------------------            
// メイン処理
// ----------------------------------------------------------------
static void print_usage() {
    cout << "--------------------------------------" << endl;
    cout << "generate_tfmovr4anime v1.0.0 by Ikotas" << endl;
    cout << "--------------------------------------" << endl;
    cout << "Usage: generate_tfmovr4anime.exe [options] TFM_output_file" << endl << endl;
    cout << "Options:" << endl;
    cout << "  -t <int>    threshold_min_p (default: 6)" << endl;
    cout << "  -r <float>  ratio_threshold (default: 0.9)" << endl;
    cout << "  -c <float>  consistency_acc (default: 0.8)" << endl;
    cout << "  -w <int>    window_size_val (default: 100)" << endl;
    cout << "  -m <int>    min_segment_len (default: 150)" << endl;
    cout << "  -b <int>    boundary_back   (default: 300)" << endl;
    cout << "  -o <file>   output filename" << endl << endl;
    cout << "The output of TFM is as follows:" << endl;
    cout << "TFM(mode=0,pp=1,slow=2,micmatching=0,output=\"x:\\path\\filename.tfm\")" << endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }

    int64_t threshold_min_p = 6;
    double  ratio_threshold = 0.9;
    double  consistency_acc = 0.8;
    size_t  window_size_val = 100;
    size_t  min_segment_len = 150;
    size_t  boundary_back = 300;
    string  tfmPathStr = "", outPathStr = "";

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) threshold_min_p = stoll(argv[++i]);
        else if (arg == "-r" && i + 1 < argc) ratio_threshold = stod(argv[++i]);
        else if (arg == "-c" && i + 1 < argc) consistency_acc = stod(argv[++i]);
        else if (arg == "-w" && i + 1 < argc) window_size_val = stoull(argv[++i]);
        else if (arg == "-m" && i + 1 < argc) min_segment_len = stoull(argv[++i]);
        else if (arg == "-b" && i + 1 < argc) boundary_back = stoull(argv[++i]);
        else if (arg == "-o" && i + 1 < argc) outPathStr = argv[++i];
        else if (arg[0] != '-') tfmPathStr = arg;
    }

    if (tfmPathStr.empty()) { print_usage(); return 1; }

    // 拡張子の自動付与を廃止し、そのまま読み込む
    fs::path tfmP = tfmPathStr;

    if (!fs::exists(tfmP)) {
        cerr << "Error: " << tfmP.filename().string() << " not found." << endl;
        cerr << "Make sure the output file for TFM is in the same folder." << endl << endl;
        cerr << "The output of TFM is as follows:" << endl;
        cerr << "TFM(mode=0,pp=1,slow=2,micmatching=0,output=\"x:\\path\\filename.tfm\")" << endl;
        return 1;
    }

    fs::path outP;
    if (outPathStr.empty()) {
        outP = tfmP; outP.replace_extension(".tfmovr");
    }
    else {
        outP = outPathStr;
        if (!outP.has_extension()) outP.replace_extension(".tfmovr");
    }

    // TFM読み込みとフォーマットチェック
    std::vector<TfmEntry> entries;
    ifstream ifs(tfmP);
    string line;
    regex re(R"((\d+)\s+([cph])\s+([\+\-])\s+\[(\d+)\])");
    smatch m;

    while (getline(ifs, line)) {
        if (line.empty() || line == "#") continue;
        if (regex_search(line, m, re)) {
            entries.push_back({
                static_cast<size_t>(stoull(m.str(1))),
                m.str(2)[0],
                m.str(3) == "+",
                stoi(m.str(4))
                });
        }
    }

    // フォーマット異常のチェック
    if (entries.empty()) {
        cerr << "Error: " << tfmP.filename().string() << " does not contain valid TFM output data." << endl;
        cerr << "Please specify a correct TFM output file." << endl << endl;
        cerr << "The output of TFM is as follows:" << endl;
        cerr << "TFM(mode=0,pp=1,slow=2,micmatching=0,output=\"x:\\path\\filename.tfm\")" << endl;
        return 1;
    }

    // 解析メインループ
    vector<pair<size_t, CyclePhase>> results;
    results.push_back({ 0, CyclePhase::Unknown });
    CyclePhase current_phase = CyclePhase::Unknown;
    size_t last_start_frame = 0;
    bool first_cycle_found = false;

    for (size_t i = 0; i < entries.size(); i += 5) {
        size_t start_idx = (i < window_size_val / 2) ? 0 : i - window_size_val / 2;
        size_t end_idx = min(entries.size(), start_idx + window_size_val);
        span<const TfmEntry> window(entries.data() + start_idx, end_idx - start_idx);

        CyclePhase detected = detect_dominant_phase(window, current_phase, threshold_min_p, ratio_threshold, consistency_acc);

        if (detected != current_phase) {
            if (!first_cycle_found && detected != CyclePhase::Unknown) {
                results.back().second = detected;
                current_phase = detected;
                last_start_frame = 0;
                first_cycle_found = true;
            }
            else if (entries[i].frame - last_start_frame >= min_segment_len) {
                size_t b_start = (i < boundary_back) ? 0 : i - boundary_back;
                size_t b_end = min(entries.size(), i + window_size_val / 2);
                span<const TfmEntry> b_window(entries.data() + b_start, b_end - b_start);

                size_t boundary = find_boundary(b_window, current_phase, detected, last_start_frame);
                if (boundary - last_start_frame >= min_segment_len) {
                    results.push_back({ boundary, detected });
                    current_phase = detected;
                    last_start_frame = boundary;
                }
            }
        }
    }

    // 出力処理
    ofstream ofs(outP);
    for (size_t i = 0; i < results.size(); ++i) {
        size_t start_f = results[i].first;
        size_t next_start = (i + 1 < results.size()) ? results[i + 1].first : 0;
        size_t end_f = (next_start > 0) ? next_start - 1 : 0;
        string pattern = generate_pattern_name(start_f, results[i].second);
        ofs << start_f << "," << end_f << " " << pattern << endl;
    }

    cout << "Successfully generated: " << outP.filename().string() << " (" << results.size() << " entries)" << endl;
    return 0;
}
