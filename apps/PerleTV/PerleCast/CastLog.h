#ifndef PERLECAST_CASTLOG_H
#define PERLECAST_CASTLOG_H

#include <string>

namespace PerleCast {

// Diagnostics for the PC (tools/inbox): appended to share/inbox/perletv.txt. Never blocks, never fails.
void CastLogLine(const std::string& text);

} // namespace PerleCast

#endif
