#pragma once

#include <string>

struct Ser2sockEndpoint {
    std::string host;
    std::string service;
};

bool ad2_parse_ser2sock_endpoint(const std::string &input, Ser2sockEndpoint &endpoint);
