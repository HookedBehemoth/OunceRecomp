// (direct back end, M3b) The translator's switches as the frozen back end runs them: every one unset. The fork reads no
// environment (docs/direct/m3-interface.md, Rules: no getenv in gpu/direct but HOS_DBG); a switch is a constant now.
#pragma once

namespace Common {
inline const char* NoEnv(const char*) { return nullptr; }
}  // namespace Common
