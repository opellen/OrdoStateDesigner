#pragma once

// The single app version: shown in the window title and About box, used in the routing
// trace log's file name, and read by tools/state-designer-mcp/server.py by regex, so keep
// the `kAppVersion[] = "..."` spelling. src/constants/ includes no project headers.

namespace app {

inline constexpr char kAppVersion[] = "0.1.0";

}  // namespace app
