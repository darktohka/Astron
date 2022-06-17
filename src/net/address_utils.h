#include <uvw.hpp>

bool is_valid_address(const std::string &hostspec);

std::vector<uvw::socket_address> resolve_address(const std::string &hostspec, uint16_t port, const std::shared_ptr<uvw::loop> &loop);
