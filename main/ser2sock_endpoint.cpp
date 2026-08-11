#include "ser2sock_endpoint.h"

#include <cstdlib>

bool ad2_parse_ser2sock_endpoint(const std::string &input, Ser2sockEndpoint &endpoint)
{
    endpoint = {};
    bool bracketed = false;
    if (!input.empty() && input.front() == '[') {
        const size_t close_bracket = input.find(']');
        if (close_bracket != std::string::npos && close_bracket + 1 < input.size() &&
                input[close_bracket + 1] == ':') {
            endpoint.host = input.substr(1, close_bracket - 1);
            endpoint.service = input.substr(close_bracket + 2);
            bracketed = true;
        }
    } else {
        const size_t separator = input.rfind(':');
        if (separator != std::string::npos) {
            endpoint.host = input.substr(0, separator);
            endpoint.service = input.substr(separator + 1);
        }
    }

    char *port_end = nullptr;
    const long port = strtol(endpoint.service.c_str(), &port_end, 10);
    return !endpoint.host.empty() && !endpoint.service.empty() &&
           (bracketed || endpoint.host.find(':') == std::string::npos) &&
           port_end != endpoint.service.c_str() && *port_end == '\0' &&
           port >= 1 && port <= 65535;
}
