#include "config/ButtonBarModel.h"

#include <algorithm>

namespace openscrt::config {

const char* toString(ButtonActionType type) noexcept {
    switch (type) {
        case ButtonActionType::kSendString:
            return "send";
        case ButtonActionType::kRunScript:
            return "script";
        case ButtonActionType::kMenuFunction:
            return "menu";
        case ButtonActionType::kOpenSession:
            return "session";
    }
    return "send";
}

std::optional<ButtonActionType> buttonActionTypeFromString(const std::string& text) {
    if (text == "send") {
        return ButtonActionType::kSendString;
    }
    if (text == "script") {
        return ButtonActionType::kRunScript;
    }
    if (text == "menu") {
        return ButtonActionType::kMenuFunction;
    }
    if (text == "session") {
        return ButtonActionType::kOpenSession;
    }
    return std::nullopt;
}

std::size_t ButtonBarModel::groupIndexOf(const std::string& name) const noexcept {
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].name == name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

std::size_t ButtonBarModel::addGroup(std::string name) {
    const std::size_t existing = groupIndexOf(name);
    if (existing != static_cast<std::size_t>(-1)) {
        return existing;
    }
    ButtonGroup group;
    group.name = std::move(name);
    groups.push_back(std::move(group));
    return groups.size() - 1;
}

bool ButtonBarModel::removeGroup(std::size_t index) {
    if (index >= groups.size()) {
        return false;
    }
    groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool ButtonBarModel::addButton(std::size_t group, ButtonAction action) {
    if (group >= groups.size()) {
        return false;
    }
    groups[group].buttons.push_back(std::move(action));
    return true;
}

bool ButtonBarModel::removeButton(std::size_t group, std::size_t button) {
    if (group >= groups.size() || button >= groups[group].buttons.size()) {
        return false;
    }
    auto& buttons = groups[group].buttons;
    buttons.erase(buttons.begin() + static_cast<std::ptrdiff_t>(button));
    return true;
}

}  // namespace openscrt::config
