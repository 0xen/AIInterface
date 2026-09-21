#include "stt/confidence.h"

#include "json.hpp"
#include "llm/json_shape.h"

namespace aii {

std::vector<float> ys_probs_from_json(const char* json) {
  std::vector<float> out;
  if (!json || !*json) return out;
  const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
  if (j.is_discarded()) return out;
  const nlohmann::json& p = member(j, "ys_probs");
  if (!p.is_array()) return out;
  out.reserve(p.size());
  for (const auto& v : p) {
    if (!v.is_number()) return {};  // a shape this does not understand at all
    out.push_back(v.get<float>());
  }
  return out;
}

}  // namespace aii
