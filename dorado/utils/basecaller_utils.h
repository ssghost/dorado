#include <optional>
#include <string>
#include <unordered_set>

namespace dorado::utils {
std::optional<std::unordered_set<std::string>> load_read_list(std::string read_list);
}
