#pragma once
// 模型清单：内置一份（编译进 exe），磁盘上的 <modelDir>\registry.ini 可逐项覆盖。
//
// 设计目标：新增/换模型不需要重新编译；自建镜像只要改 registry.ini 里的 url。

#include <cstdint>
#include <string>
#include <vector>

namespace util {

struct ModelFile {
    std::string file;      // modelDir 下的文件名
    std::string label;     // 给人看的名字
    std::string kind;      // det / rec / cls
    std::string url;       // 官方下载直链（空 = 无在线源，需手动放置）
    std::string altUrl;    // 备用直链（可选，镜像）
    std::int64_t size = 0; // 字节数，0 = 未知
    std::string sha256;    // 期望 SHA256，空 = 不校验
};

/// 内置清单 + <modelDir>\registry.ini 覆盖（后者存在时优先生效）。
std::vector<ModelFile> LoadModelRegistry(const std::string& modelDir);

/// 按文件名找条目，找不到返回 nullptr。
const ModelFile* FindModelFile(const std::vector<ModelFile>& list, const std::string& file);

/// UTF-8 目录 + 文件名拼接（自动补反斜杠）。
std::string JoinPath(const std::string& dir, const std::string& name);

/// 文件存在且非空。
bool FileReady(const std::string& path);

/// 保证模型目录可用：优先用 preferred；不存在/不可写时退回 %LOCALAPPDATA%\pureocr\models。
/// 返回最终可写目录（UTF-8，已创建）。
std::string ResolveModelDir(const std::string& preferred);

}  // namespace util