#pragma once

namespace live_assistant {

enum class ModuleState {
    UNINITIALIZED,
    INITIALIZED,
    RUNNING,
    STOPPED,
    ERROR
};

template<typename ConfigType = void>
class ModuleLifecycle {
public:
    virtual ~ModuleLifecycle() = default;
    virtual ErrorCode initialize(const ConfigType& config) = 0;
    virtual ErrorCode start() = 0;
    virtual ErrorCode stop() = 0;
    virtual ErrorCode shutdown() = 0;
};

template<>
class ModuleLifecycle<void> {
public:
    virtual ~ModuleLifecycle() = default;
    virtual ErrorCode initialize() = 0;
    virtual ErrorCode start() = 0;
    virtual ErrorCode stop() = 0;
    virtual ErrorCode shutdown() = 0;
};

} // namespace live_assistant