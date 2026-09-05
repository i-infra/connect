// Nonolith Connect test runner
// Defines the doctest main and the server globals normally owned by
// server.cpp (which is not linked into the test binary).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../dataserver.hpp"

std::set<device_ptr> devices;
boost::asio::io_context io;
bool debugFlag = false;

Event device_list_changed;
