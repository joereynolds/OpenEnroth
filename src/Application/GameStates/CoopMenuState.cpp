#include "CoopMenuState.h"

#include <cassert>
#include <charconv>
#include <exception>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <algorithm>

#include "Engine/AssetsManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineGlobals.h"
#include "Engine/Coop/CoopProtocol.h"
#include "Engine/Coop/CoopServer.h"
#include "Engine/Coop/CoopSession.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Renderer/Renderer.h"
#include "Engine/Resources/EngineFileSystem.h"

#include "GUI/GUIButton.h"
#include "GUI/GUIFont.h"
#include "GUI/GUIMessageQueue.h"
#include "GUI/GUIWindow.h"
#include "GUI/UI/UIPopup.h"

#include "Io/KeyboardInputHandler.h"
#include "Io/Mouse.h"

#include "Utility/String/Format.h"


enum class CoopMenuField {
    COOP_MENU_FIELD_NAME = 0,
    COOP_MENU_FIELD_ADDRESS = 1,
    COOP_MENU_FIELD_PORT = 2,
};
using enum CoopMenuField;

/**
 * What the co-op menu shows: a choice between hosting and joining, or the form for one of them.
 */
enum class CoopMenuPage {
    COOP_MENU_PAGE_CHOOSE,
    COOP_MENU_PAGE_HOST_NEW, // Everyone makes a party in a lobby, then the game starts.
    COOP_MENU_PAGE_HOST_SAVED, // The host loads a save, friends join with saves of their own.
    COOP_MENU_PAGE_JOIN,
};
using enum CoopMenuPage;

static constexpr int FIELD_COUNT = 3;
static constexpr int FIELD_MAX_LENGTH = 40;
static constexpr std::array<const char *, FIELD_COUNT> FIELD_LABELS = {"Your name", "Host address", "Port"};

static constexpr int PANEL_X = 120;
static constexpr int PANEL_Y = 110;
static constexpr int PANEL_W = 400;
static constexpr int ROW_H = 30;
static constexpr int VALUE_X = PANEL_X + 170;

/**
 * @param page                          Page of the co-op menu.
 * @return                              Fields shown on that page, top to bottom.
 */
static std::span<const int> pageFields(CoopMenuPage page) {
    static constexpr std::array<int, 2> HOST_FIELDS = {std::to_underlying(COOP_MENU_FIELD_NAME), std::to_underlying(COOP_MENU_FIELD_PORT)};
    static constexpr std::array<int, 3> JOIN_FIELDS = {std::to_underlying(COOP_MENU_FIELD_NAME), std::to_underlying(COOP_MENU_FIELD_ADDRESS),
                                                       std::to_underlying(COOP_MENU_FIELD_PORT)};
    switch (page) {
    case COOP_MENU_PAGE_HOST_NEW:
    case COOP_MENU_PAGE_HOST_SAVED: return HOST_FIELDS;
    case COOP_MENU_PAGE_JOIN: return JOIN_FIELDS;
    default: return {};
    }
}

/**
 * Window of the co-op menu, it draws the current page and owns its buttons.
 */
class GUIWindow_CoopMenu : public GUIWindow {
 public:
    GUIWindow_CoopMenu() : GUIWindow(WINDOW_COOP_MENU, {0, 0}, render->GetRenderDimensions()) {
        _background = assets->getIcon("mm6title.pcx");
        showPage(COOP_MENU_PAGE_CHOOSE);
    }

    virtual ~GUIWindow_CoopMenu() {
        assets->releaseImage(_background);
    }

    void showPage(CoopMenuPage newPage) {
        page = newPage;
        error.clear();
        DeleteButtons();
        if (page == COOP_MENU_PAGE_CHOOSE) {
            CreateButton("CoopMenu_HostNewPage", {PANEL_X + 20, rowY(0)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_ShowHostNewPage, 0);
            CreateButton("CoopMenu_HostPage", {PANEL_X + 20, rowY(1)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_ShowHostPage, 0);
            CreateButton("CoopMenu_JoinPage", {PANEL_X + 20, rowY(2)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_ShowJoinPage, 0);
            CreateButton("CoopMenu_Back", {PANEL_X + 20, rowY(3)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Back, 0, INPUT_ACTION_ESCAPE);
            return;
        }

        std::span<const int> fields = pageFields(page);
        for (size_t i = 0; i < fields.size(); i++)
            CreateButton(fmt::format("CoopMenu_Field{}", fields[i]), {PANEL_X, rowY(i)}, {PANEL_W, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_SelectField, fields[i]);
        if (page == COOP_MENU_PAGE_JOIN) {
            CreateButton("CoopMenu_Join", {PANEL_X + 20, buttonsY()}, {150, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Join, 0);
        } else {
            CreateButton("CoopMenu_Host", {PANEL_X + 20, buttonsY()}, {150, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Host, 0);
        }
        CreateButton("CoopMenu_Back", {PANEL_X + 280, buttonsY()}, {100, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Back, 0);
    }

    virtual void Update() override {
        render->DrawQuad2D(_background, {0, 0});
        DrawPopupWindow(PANEL_X - 20, PANEL_Y - 50, PANEL_W + 40, buttonsY() - PANEL_Y + 175);

        GUIFont *font = assets->pFontCreate.get();
        Pointi mousePos = mouse->position();
        auto drawButton = [&](std::string_view id, std::string_view text) {
            GUIButton *button = findButton(id);
            assert(button);
            Color color = button->Contains(mousePos) ? colorTable.LaserLemon : colorTable.White;
            DrawText(font, button->rect.topLeft(), color, text, frameRect);
        };
        auto drawHint = [&](std::string_view line1, std::string_view line2) {
            DrawText(font, {PANEL_X, buttonsY() + ROW_H}, colorTable.Gray, line1, frameRect);
            DrawText(font, {PANEL_X, buttonsY() + ROW_H + 20}, colorTable.Gray, line2, frameRect);
        };

        switch (page) {
        case COOP_MENU_PAGE_CHOOSE:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Multiplayer", frameRect);
            drawButton("CoopMenu_HostNewPage", "Host a new game");
            drawButton("CoopMenu_HostPage", "Host a saved game");
            drawButton("CoopMenu_JoinPage", "Join a friend's game");
            drawButton("CoopMenu_Back", "Back");
            drawHint("One player hosts, the others join.", "");
            break;
        case COOP_MENU_PAGE_HOST_NEW:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Host a new game", frameRect);
            drawFields(font);
            drawButton("CoopMenu_Host", "Make your party");
            drawButton("CoopMenu_Back", "Back");
            drawHint("Everyone makes a party, then you start.", "Friends join with your IP address.");
            break;
        case COOP_MENU_PAGE_HOST_SAVED:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Host a saved game", frameRect);
            drawFields(font);
            drawButton("CoopMenu_Host", "Pick the save");
            drawButton("CoopMenu_Back", "Back");
            drawHint("Load your game, friends join with their", "saves. Friends join with your IP address.");
            break;
        case COOP_MENU_PAGE_JOIN:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Join a friend's game", frameRect);
            drawFields(font);
            drawButton("CoopMenu_Join", "Join");
            drawButton("CoopMenu_Back", "Back");
            drawHint("You pick your party once connected.", "");
            break;
        }

        if (!error.empty())
            DrawText(font, {PANEL_X, buttonsY() + 3 * ROW_H}, colorTable.Red, error, frameRect);
    }

    std::array<std::string, FIELD_COUNT> fields = {"Player", "", fmt::format("{}", COOP_DEFAULT_PORT)};
    CoopMenuPage page = COOP_MENU_PAGE_CHOOSE;
    int selected = -1; // Field being typed into, -1 on the choice page.
    std::string error;

 private:
    void drawFields(GUIFont *font) {
        std::span<const int> shown = pageFields(page);
        for (size_t i = 0; i < shown.size(); i++) {
            int field = shown[i];
            Color color = field == selected ? colorTable.LaserLemon : colorTable.White;
            DrawText(font, {PANEL_X, rowY(i)}, color, FIELD_LABELS[field], frameRect);
            DrawText(font, {VALUE_X, rowY(i)}, color, fields[field] + (field == selected ? "_" : ""), frameRect);
        }
    }

    GUIButton *findButton(std::string_view id) {
        auto pos = std::ranges::find_if(vButtons, [&](GUIButton *button) { return button->id == id; });
        return pos == vButtons.end() ? nullptr : *pos;
    }

    static int rowY(int row) {
        return PANEL_Y + row * ROW_H;
    }

    int buttonsY() const {
        int rows = page == COOP_MENU_PAGE_CHOOSE ? 4 : pageFields(page).size(); // The choice page has four buttons and no fields.
        return PANEL_Y + rows * ROW_H + 15;
    }

    GraphicsImage *_background = nullptr;
};

CoopMenuState::CoopMenuState() = default;
CoopMenuState::~CoopMenuState() = default;

FsmAction CoopMenuState::enter() {
    _window = std::make_unique<GUIWindow_CoopMenu>();
    return FsmAction::none();
}

FsmAction CoopMenuState::update() {
    if (_window->page != COOP_MENU_PAGE_CHOOSE) {
        _window->fields[_window->selected] = keyboardInputHandler->GetTextInput();
        if (_window->keyboard_input_status == WINDOW_INPUT_CONFIRMED) {
            std::span<const int> shown = pageFields(_window->page);
            auto pos = std::ranges::find(shown, _window->selected);
            selectField(shown[(pos - shown.begin() + 1) % shown.size()]);
        } else if (_window->keyboard_input_status == WINDOW_INPUT_NONE) {
            showPage(COOP_MENU_PAGE_CHOOSE); // Escape ends text input, it's the only way to get here.
            return FsmAction::none();
        }
    }

    while (engine->_messageQueue->haveMessages()) {
        UIMessageType message;
        int param = 0;
        engine->_messageQueue->popMessage(&message, &param, nullptr);
        switch (message) {
        case UIMSG_CoopMenu_ShowHostNewPage:
            showPage(COOP_MENU_PAGE_HOST_NEW);
            break;
        case UIMSG_CoopMenu_ShowHostPage:
            showPage(COOP_MENU_PAGE_HOST_SAVED);
            break;
        case UIMSG_CoopMenu_ShowJoinPage:
            showPage(COOP_MENU_PAGE_JOIN);
            break;
        case UIMSG_CoopMenu_SelectField:
            selectField(param);
            break;
        case UIMSG_CoopMenu_Host:
            if (!host())
                break;
            if (_window->page == COOP_MENU_PAGE_HOST_NEW) {
                coopSession->server()->openLobby();
                SetCurrentMenuID(MENU_NEWGAME);
                return FsmAction::transition("newGame");
            }
            SetCurrentMenuID(MENU_SAVELOAD);
            return FsmAction::transition("loadGame");
        case UIMSG_CoopMenu_Join:
            if (join())
                return FsmAction::transition("back");
            break;
        case UIMSG_CoopMenu_Back:
        case UIMSG_Escape:
            if (_window->page == COOP_MENU_PAGE_CHOOSE)
                return FsmAction::transition("back");
            showPage(COOP_MENU_PAGE_CHOOSE);
            break;
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

void CoopMenuState::showPage(CoopMenuPage page) {
    keyboardInputHandler->EndTextInput(_window.get());
    _window->showPage(page);
    _window->selected = -1;
    if (page == COOP_MENU_PAGE_HOST_NEW || page == COOP_MENU_PAGE_HOST_SAVED)
        selectField(std::to_underlying(COOP_MENU_FIELD_NAME));
    if (page == COOP_MENU_PAGE_JOIN)
        selectField(std::to_underlying(COOP_MENU_FIELD_ADDRESS));
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
        coopSession = CoopSession::host(*port, COOP_MAX_CLIENTS, _window->fields[std::to_underlying(COOP_MENU_FIELD_NAME)]);
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
        coopSession.reset();
        coopSession = CoopSession::join(address, *port, _window->fields[std::to_underlying(COOP_MENU_FIELD_NAME)], Blob());
        coopSession->choosePartyAfterJoining();
    } catch (const std::exception &e) {
        _window->error = e.what();
        return false;
    }
    return true;
}
