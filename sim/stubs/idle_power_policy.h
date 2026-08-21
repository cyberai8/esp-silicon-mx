#pragma once

enum class IdlePowerSession { Standby, Home, App };

inline void IdlePower_Attach(IdlePowerSession /*s*/, bool /*reset_activity*/ = false) {}
inline void IdlePower_Detach(IdlePowerSession /*s*/) {}
inline void IdlePower_NotifyActivity() {}
