#pragma once

// Minimal `--key value` / `--flag` parser. Everything after a bare `--` is kept
// verbatim (the DPDK path hands it to rte_eal_init).

#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bl {

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--") {
                for (++i; i < argc; ++i) rest_.push_back(argv[i]);
                break;
            }
            if (a.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument: " + a);
            a = a.substr(2);
            if (const auto eq = a.find('='); eq != std::string::npos) {
                kv_[a.substr(0, eq)] = a.substr(eq + 1);
            } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                kv_[a] = argv[++i];
            } else {
                kv_[a] = "1";
            }
        }
    }
    bool has(const std::string& k) const { return kv_.count(k) != 0; }
    std::string str(const std::string& k, const std::string& def = {}) const {
        const auto it = kv_.find(k);
        return it == kv_.end() ? def : it->second;
    }
    std::int64_t i64(const std::string& k, std::int64_t def) const {
        const auto it = kv_.find(k);
        return it == kv_.end() ? def : std::strtoll(it->second.c_str(), nullptr, 10);
    }
    double f64(const std::string& k, double def) const {
        const auto it = kv_.find(k);
        return it == kv_.end() ? def : std::strtod(it->second.c_str(), nullptr);
    }
    const std::vector<std::string>& rest() const { return rest_; }

private:
    std::map<std::string, std::string> kv_;
    std::vector<std::string> rest_;
};

}  // namespace bl
