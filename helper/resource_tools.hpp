#pragma once

#include "observer.hpp"

namespace observer {
// On-demand inspection only. Resource operations never modify a target process
// and return the same structured fields/text schema for every resource kind.
Json resource_tool(const Json& request);
}
