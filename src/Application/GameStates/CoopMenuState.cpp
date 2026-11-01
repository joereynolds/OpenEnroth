#include "CoopMenuState.h"

#include <charconv>
#include <exception>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "Engine/AssetsManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineGlobals.h"
#include "Engine/Coop/CoopProtocol.h"
#include "Engine/Coop/CoopSession.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Renderer/Renderer.h"
#include "Engine/Resources/EngineFileSystem.h"

#include "GUI/GUIFont.h"
#include "GUI/GUIMessageQueue.h"
#include "GUI/GUIWindow.h"
#include "GUI/UI/UIPopup.h"

#include "Io/KeyboardInputHandler.h"

#include "Utility/String/Format.h"

enum class CoopMenuField {
    COOP_MENU_FIELD_NAME = 0,
    COOP_MENU_FIELD_ADDRESS = 1,
    COOP_MENU_FIELD_PORT = 2,
    COOP_MENU_FIELD_PARTY = 3,
};
using enum CoopMenuField;

static constexpr int FIELD_COUNT = 4;
static constexpr int FIELD_MAX_LENGTH = 40;
static constexpr std::array<const char *, FIELD_COUNT> FIELD_LABELS = {"Name", "Host address", "Port", "Party save"};

static constexpr int PANEL_X = 120;
static constexpr int PANEL_Y = 110;
static constexpr int PANEL_W = 400;
static constexpr int ROW_H = 30;
static constexpr int VALUE_X = PANEL_X + 150;

/**
 * Window of the co-op menu, it draws the fields and owns the buttons.
 */
class GUIWindow_CoopMenu : public GUIWindow {
 public:
    GUIWindow_CoopMenu() : GUIWindow(WINDOW_COOP_MENU, {0, 0}, render->GetRenderDimensions()) {
        _background = assets->getIcon("mm6title.pcx");
        for (int i = 0; i < FIELD_COUNT; i++)
            CreateButton(fmt::format("CoopMenu_Field{}", i), {PANEL_X, fieldY(i)}, {PANEL_W, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_SelectField, i);
        CreateButton("CoopMenu_Host", {PANEL_X + 20, buttonsY()}, {100, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Host, 0);
        CreateButton("CoopMenu_Join", {PANEL_X + 150, buttonsY()}, {100, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Join, 0);
        CreateButton("CoopMenu_Back", {PANEL_X + 280, buttonsY()}, {100, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Back, 0);
    }

    virtual ~GUIWindow_CoopMenu() {
        assets->releaseImage(_background);
    }

    virtual void Update() override {
        render->DrawQuad2D(_background, {0, 0});
        DrawPopupWindow(PANEL_X - 20, PANEL_Y - 50, PANEL_W + 40, buttonsY() - PANEL_Y + 120);

        GUIFont *font = assets->pFontCreate.get();
        DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Co-op game", frameRect);
        for (int i = 0; i < FIELD_COUNT; i++) {
            Color color = i == selected ? colorTable.LaserLemon : colorTable.White;
            DrawText(font, {PANEL_X, fieldY(i)}, color, FIELD_LABELS[i], frameRect);
            std::string value = fields[i] + (i == selected ? "_" : "");
            DrawText(font, {VALUE_X, fieldY(i)}, color, value, frameRect);
        }
        DrawText(font, {PANEL_X + 20, buttonsY()}, colorTable.White, "[Host]", frameRect);
        DrawText(font, {PANEL_X + 150, buttonsY()}, colorTable.White, "[Join]", frameRect);
        DrawText(font, {PANEL_X + 280, buttonsY()}, colorTable.White, "[Back]", frameRect);
        DrawText(font, {PANEL_X, buttonsY() + ROW_H}, colorTable.Gray, "Enter: next field. The party save is optional.", frameRect);
        if (!error.empty())
            DrawText(font, {PANEL_X, buttonsY() + 2 * ROW_H}, colorTable.Red, error, frameRect);
    }

    std::array<std::string, FIELD_COUNT> fields = {"Player", "", fmt::format("{}", COOP_DEFAULT_PORT), ""};
    int selected = std::to_underlying(COOP_MENU_FIELD_ADDRESS);
    std::string error;

 private:
    static int fieldY(int field) {
        return PANEL_Y + field * ROW_H;
    }

    static int buttonsY() {
        return PANEL_Y + FIELD_COUNT * ROW_H + 15;
    }

    GraphicsImage *_background = nullptr;
};

CoopMenuState::CoopMenuState() = default;
CoopMenuState::~CoopMenuState() = default;

FsmAction CoopMenuState::enter() {
    _window = std::make_unique<GUIWindow_CoopMenu>();
    selectField(_window->selected);
    return FsmAction::none();
}

FsmAction CoopMenuState::update() {
    _window->fields[_window->selected] = keyboardInputHandler->GetTextInput();

    if (_window->keyboard_input_status == WINDOW_INPUT_CONFIRMED) {
        selectField((_window->selected + 1) % FIELD_COUNT);
    } else if (_window->keyboard_input_status == WINDOW_INPUT_NONE) {
        return FsmAction::transition("back"); // Escape ends text input, it's the only way to get here.
    }

    while (engine->_messageQueue->haveMessages()) {
        UIMessageType message;
        int param = 0;
        engine->_messageQueue->popMessage(&message, &param, nullptr);
        switch (message) {
        case UIMSG_CoopMenu_SelectField:
            selectField(param);
            break;
        case UIMSG_CoopMenu_Host:
            if (host())
                return FsmAction::transition("back");
            break;
        case UIMSG_CoopMenu_Join:
            if (join())
                return FsmAction::transition("back");
            break;
        case UIMSG_CoopMenu_Back:
        case UIMSG_Escape:
            return FsmAction::transition("back");
        default:
            break;
        }
    }
    return FsmAction::none();
}

void CoopMenuState::exit() {
    keyboardInputHandler->EndTextInput(_window.get());
    _window.reset();
}

void CoopMenuState::selectField(int field) {
    _window->selected = field;
    TextInputType type = field == std::to_underlying(COOP_MENU_FIELD_PORT) ? TEXT_INPUT_NUMBER : TEXT_INPUT_TEXT;
    keyboardInputHandler->StartTextInput(type, FIELD_MAX_LENGTH, _window.get());
    keyboardInputHandler->SetTextInput(_window->fields[field]);
}

static std::optional<int> parsePort(const std::string &text) {
    int port = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc() || end != text.data() + text.size() || port < 1 || port > 65535)
        return std::nullopt;
    return port;
}

bool CoopMenuState::host() {
    std::optional<int> port = parsePort(_window->fields[std::to_underlying(COOP_MENU_FIELD_PORT)]);
    if (!port) {
        _window->error = "The port has to be a number from 1 to 65535";
        return false;
    }

    try {
        coopSession.reset();
        coopSession = CoopSession::host(*port, COOP_MAX_CLIENTS);
    } catch (const std::exception &e) {
        _window->error = e.what();
        return false;
    }
    return true;
}

bool CoopMenuState::join() {
    const std::string &address = _window->fields[std::to_underlying(COOP_MENU_FIELD_ADDRESS)];
    std::optional<int> port = parsePort(_window->fields[std::to_underlying(COOP_MENU_FIELD_PORT)]);
    if (address.empty()) {
        _window->error = "Type the host's address first";
        return false;
    }
    if (!port) {
        _window->error = "The port has to be a number from 1 to 65535";
        return false;
    }

    try {
        Blob guestSave;
        const std::string &party = _window->fields[std::to_underlying(COOP_MENU_FIELD_PARTY)];
        if (!party.empty())
            guestSave = Blob::copy(ufs->read(fmt::format("saves/{}", party)));
        coopSession.reset();
        coopSession = CoopSession::join(address, *port, _window->fields[std::to_underlying(COOP_MENU_FIELD_NAME)], std::move(guestSave));
    } catch (const std::exception &e) {
        _window->error = e.what();
        return false;
    }
    return true;
}
