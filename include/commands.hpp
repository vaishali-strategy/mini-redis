#pragma once
// Turns a parsed command into a reply. No sockets here, so it can be tested alone.

#include <string>
#include <vector>

#include "store.hpp"

// args[0] is the command name (any case); the rest are its arguments.
// Appends exactly one RESP reply to `out`.
void execute_command(Store& store, const std::vector<std::string>& args, std::string& out);
