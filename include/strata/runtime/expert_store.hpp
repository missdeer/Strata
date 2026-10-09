#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace strata::runtime {
struct ExpertLayer {
    int gate_up = 0, down = 0;
    uint64_t offset = 0, bytes = 0, gate_bytes = 0, down_bytes = 0;
    uint64_t role_offset[3]{};
    std::filesystem::path role_file[3];
};

class MappedFile;
class ExpertStore {
public:
    using Buffer = std::shared_ptr<std::vector<uint8_t>>;
    ExpertStore(const std::filesystem::path& pack, const std::filesystem::path& native, uint64_t budget);
    ~ExpertStore();
    ExpertStore(const ExpertStore&) = delete;
    ExpertStore& operator=(const ExpertStore&) = delete;
    const std::vector<ExpertLayer>& layers() const { return layers_; }
    Buffer get(int layer, int expert);
    uint64_t fingerprint();
    void preload(int index, int count, const std::string& assignment, const std::function<bool()>& cancelled = {});
    uint64_t cached_bytes() const { return cached_; }
private:
    Buffer read(int layer, int expert);
    struct Cached { Buffer buffer; std::list<int>::iterator position; };
    std::vector<ExpertLayer> layers_;
    std::map<std::filesystem::path, std::unique_ptr<MappedFile>> maps_;
    std::map<int, Cached> cache_;
    std::list<int> lru_;
    std::filesystem::path packed_;
    uint64_t budget_ = 0, cached_ = 0;
};
} // namespace strata::runtime
