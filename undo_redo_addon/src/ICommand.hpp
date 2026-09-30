#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>
#include <reshade.hpp>

enum class CommandDomain : uint8_t { ShaderState = 0, WindowLayout = 1, AddonCustom = 2, Count = 3 };

constexpr size_t kHistoryLimit = 1000;

constexpr uint32_t kCommandTypeUniform         = 0x554E4946;
constexpr uint32_t kCommandTypeCompoundUniform = 0x43504E44;
constexpr uint32_t kCommandTypeTechniqueToggle = 0x54454348;
constexpr uint32_t kCommandTypeWindowTransform = 0x57494E44;
constexpr uint32_t kCommandTypeExternalAddon   = 0x4558544E;

class ICommand {
public:
    virtual ~ICommand() = default;

    virtual void execute(reshade::api::effect_runtime *runtime = nullptr) = 0;
    virtual void undo(reshade::api::effect_runtime *runtime = nullptr) = 0;

    virtual CommandDomain domain() const = 0;
    virtual std::string get_label() const = 0;
    virtual std::string get_delta_preview() const { return ""; }
    virtual uint32_t type_id() const = 0;

    virtual int32_t merge_id() const { return -1; }
    virtual bool merge_with(const ICommand *) { return false; }
};
