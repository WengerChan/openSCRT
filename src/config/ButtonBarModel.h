// The button bar (P2-T10).
//
// SecureCRT puts a row of buttons at the bottom of the terminal. Every button does one
// thing -- send a string, run a script, call a menu function, open a session -- and the
// row is grouped into tabs, so there can be more than one set of buttons.
//
// The groups are configuration, not decoration: the user builds them, they are saved
// with the sessions, and they come back the next time the window opens. That is why the
// model lives here, next to the session tree, and not in the widget that draws it.
//
// This file is free of Qt on purpose, like the session tree: the rules -- what an
// action is, what a group holds -- are worth testing without a display.

#ifndef OPENCRT_CONFIG_BUTTON_BAR_MODEL_H_
#define OPENCRT_CONFIG_BUTTON_BAR_MODEL_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openscrt::config {

// What a button does when it is pressed.
enum class ButtonActionType : std::uint8_t {
    kSendString,    // send the text to the session
    kRunScript,     // run a Python script
    kMenuFunction,  // call a menu function
    kOpenSession,   // open a saved session
};

[[nodiscard]] const char* toString(ButtonActionType type) noexcept;
[[nodiscard]] std::optional<ButtonActionType> buttonActionTypeFromString(const std::string& text);

struct ButtonAction {
    ButtonActionType type{ButtonActionType::kSendString};
    // What the action does, in the words of its type: the text to send, the path of the
    // script, the path of the menu function, or the id of the session.
    std::string value;
    // The caption of the button. An empty caption means the value is shown instead.
    std::string label;
};

struct ButtonGroup {
    std::string name;
    std::vector<ButtonAction> buttons;
};

// The whole button bar: a list of groups, in the order they are shown.
struct ButtonBarModel {
    std::vector<ButtonGroup> groups;

    [[nodiscard]] bool empty() const noexcept { return groups.empty(); }

    // The index of the group with that name, or -1.
    [[nodiscard]] std::size_t groupIndexOf(const std::string& name) const noexcept;

    // Adds a group and returns its index. A group with the same name is not added twice,
    // because two tabs with the same name are impossible to tell apart.
    std::size_t addGroup(std::string name);

    // Removes a group. Returns false when the index is not there.
    bool removeGroup(std::size_t index);

    // Adds a button to a group. Returns false when the group is not there.
    bool addButton(std::size_t group, ButtonAction action);

    // Removes a button from a group. Returns false when either index is not there.
    bool removeButton(std::size_t group, std::size_t button);
};

}  // namespace openscrt::config

#endif  // OPENCRT_CONFIG_BUTTON_BAR_MODEL_H_
