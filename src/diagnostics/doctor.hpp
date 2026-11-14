#pragma once
#include <ostream>

namespace wf {
// Environment health check. Returns 0 (ok / warnings only), 3 when no usable backend exists.
int runDoctor(std::ostream& os);
}  // namespace wf
