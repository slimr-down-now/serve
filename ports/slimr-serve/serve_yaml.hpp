#pragma once
#include "serve_http.hpp"
#include "yaml.hpp"

#include <string_view>

namespace slimr {

/** Returns a YamlCodec backed by slimr::parse_yaml<Json> and slimr::to_yaml. */
inline YamlCodec make_yaml_codec() {
    return YamlCodec{
        .parse = [](std::string_view text) { return parse_yaml<Json>(text); },
        .emit = [](const Json &val) { return to_yaml(val); },
    };
}

/** Attaches opt-in YAML request/response support to ServeHttpOptions. */
inline void enable_yaml(ServeHttpOptions &opts) {
    opts.yaml = make_yaml_codec();
}

} // namespace slimr
