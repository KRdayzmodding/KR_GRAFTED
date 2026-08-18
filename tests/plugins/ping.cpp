#include "ping.hpp"

namespace seraph {

std::string proof_line(std::int32_t token, std::int32_t reply) {
    return "SERAPH_GRAFT token=" + std::to_string(token) +
           " reply=" + std::to_string(reply);
}

}  // namespace seraph
