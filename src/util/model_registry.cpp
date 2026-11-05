#include "util/model_registry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "util/utf8_io.h"
#include "util/platform.h"

namespace util {
namespace {

// 内置清单：与 models/registry.ini 保持一致（改一处记得改另一处）。
// 所有 URL 都实测可用；文件均与仓库 models/ 下的文件逐字节相同（SHA256 已核对）。
const ModelFile kBuiltin[] = {
    {"ch_PP-OCRv5_det_infer.onnx", "v5 移动版·检测", "det",
     "https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_det_onnx/resolve/main/inference.onnx",
     "", 4826518, "A431985659DC921974177A95ADCFBB90FD9E51989A5E04D70D0B75F597B6E61D"},
    {"ch_PP-OCRv5_rec_infer.onnx", "v5 移动版·识别", "rec",
     "https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_rec_onnx/resolve/main/inference.onnx",
     "", 16534782, "DA72DC72CA4DC220DF0DFDE68C1DEDC31C58D3E76A25871122E5056227D50092"},
    {"ch_PP-OCRv4_det_infer.onnx", "v4 移动版·检测", "det",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx",
     "", 4745517, "D2A7720D45A54257208B1E13E36A8479894CB74155A5EFE29462512D42F49DA9"},
    {"ch_PP-OCRv4_rec_infer.onnx", "v4 移动版·识别", "rec",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
     "", 10857958, "48FC40F24F6D2A207A2B1091D3437EB3CC3EB6B676DC3EF9C37384005483683B"},
    {"ch_PP-OCRv3_det_infer.onnx", "v3 移动版·检测", "det",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv3/ch_PP-OCRv3_det_infer.onnx",
     "", 2432880, "3439588C030FAEA393A54515F51E983D8E155B19A2E8ABA7891934C1CF0DE526"},
    {"ch_PP-OCRv3_rec_infer.onnx", "v3 移动版·识别", "rec",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv3/ch_PP-OCRv3_rec_infer.onnx",
     "", 10690752, "897A3EDEDB38FEE0DAE2C1CCEE38241F37DF202C9509E3ABCA02E9217C5EE615"},
    {"ch_ppocr_mobile_v2.0_cls_infer.onnx", "方向分类（0/180°）", "cls",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx",
     "", 585532, "E47ACEDF663230F8863FF1AB0E64DD2D82B838FCEB5957146DAB185A89D6215C"},
    {"ch_PP-OCRv4_det_server_infer.onnx", "v4 服务器版·检测", "det",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_server_infer.onnx",
     "", 113352104, ""},
    {"ch_PP-OCRv4_rec_server_infer.onnx", "v4 服务器版·识别", "rec",
     "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_server_infer.onnx",
     "", 90530732, ""},
    {"ch_PP-OCRv5_det_server_infer.onnx", "v5 服务器版·检测", "det",
     "https://huggingface.co/bukuroo/PPOCRv5-ONNX/resolve/main/ppocrv5-server-det.onnx",
     "", 87697340, "A61170E16A239059E715DEBF2FF37AEB7597597848570D25EF8BA36BE80AD418"},
    {"ch_PP-OCRv5_rec_server_infer.onnx", "v5 服务器版·识别", "rec",
     "https://huggingface.co/bukuroo/PPOCRv5-ONNX/resolve/main/ppocrv5-server-rec.onnx",
     "", 84137438, "BCDC8836FE5FB70F18C768F2B99CC37F5D45705A9DC78E65F6DC3AF869CB2E40"},
};

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/// registry.ini 覆盖：按出现的节名匹配内置条目，只覆盖非空字段；未匹配的节追加为新条目。
void ApplyIni(const std::string& iniPath, std::vector<ModelFile>* list) {
    FILE* f = OpenUtf8(iniPath, L"rb");
    if (!f) return;

    std::string content;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    std::fclose(f);
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);  // 去掉可能存在的 UTF-8 BOM
    }

    ModelFile* cur = nullptr;
    size_t pos = 0;
    while (pos <= content.size()) {
        const size_t eol = content.find('\n', pos);
        const std::string raw = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? content.size() + 1 : eol + 1;

        const std::string line = Trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            const std::string name = Trim(line.substr(1, line.size() - 2));
            cur = nullptr;
            for (auto& m : *list) {
                if (m.file == name) {
                    cur = &m;
                    break;
                }
            }
            if (!cur) {
                ModelFile m;
                m.file = name;
                list->push_back(m);
                cur = &list->back();
            }
            continue;
        }
        if (!cur) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (val.empty()) continue;
        if (key == "url") cur->url = val;
        else if (key == "alt_url" || key == "mirror") cur->altUrl = val;
        else if (key == "label") cur->label = val;
        else if (key == "kind") cur->kind = val;
        else if (key == "size") cur->size = static_cast<long long>(std::strtoll(val.c_str(), nullptr, 10));
        else if (key == "sha256") cur->sha256 = val;
    }
}

bool DirWritable(const std::string& dir) {
#ifdef _WIN32
    const std::wstring w = Utf8ToWide(dir);
    if (w.empty()) return false;
    if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const std::wstring probe = w + L"\\.write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    if (!util::MakeDirs(dir)) return false;
    const std::string probe = dir + "/.write-test";
    FILE* f = std::fopen(probe.c_str(), "wb");
    if (!f) return false;
    std::fclose(f);
    std::remove(probe.c_str());
    return true;
#endif
}

}  // namespace

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "/" + name;
}

bool FileReady(const std::string& path) {
#ifdef _WIN32
    const std::wstring w = Utf8ToWide(path);
    if (w.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    std::memset(&fad, 0, sizeof(fad));
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    return (static_cast<std::int64_t>(fad.nFileSizeHigh) << 32 | fad.nFileSizeLow) > 0;
#else
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    return !ec && sz > 0;
#endif
}

std::vector<ModelFile> LoadModelRegistry(const std::string& modelDir) {
    std::vector<ModelFile> list(std::begin(kBuiltin), std::end(kBuiltin));
    if (!modelDir.empty()) ApplyIni(JoinPath(modelDir, "registry.ini"), &list);
    return list;
}

const ModelFile* FindModelFile(const std::vector<ModelFile>& list, const std::string& file) {
    for (const auto& m : list) {
        if (m.file == file) return &m;
    }
    return nullptr;
}

std::string ResolveModelDir(const std::string& preferred) {
    if (!preferred.empty() && DirWritable(preferred)) return preferred;

    std::string base;
    base = util::GetEnv("LOCALAPPDATA");
    if (base.empty()) base = util::GetEnv("XDG_DATA_HOME");
    if (base.empty()) base = util::GetEnv("HOME");
    if (base.empty()) base = preferred;
    const std::string dir = JoinPath(base, "pureocr/models");
    if (DirWritable(dir)) return dir;
    return preferred;
}

}  // namespace util
