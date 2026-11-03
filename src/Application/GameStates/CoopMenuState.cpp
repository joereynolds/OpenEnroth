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
#include "Engine/Coop/CoopSession.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Renderer/Renderer.h"
#include "Engine/Resources/EngineFileSystem.h"

#include "GUI/GUIButton.h"
#include "GUI/GUIFont.h"
#include "GUI/GUIMessageQueue.h"
#include "GUI/GUIWindow.h"
#include "GUI/UI/UIPopup.h"
#include "GUI/UI/UISaveLoad.h"

#include "Io/KeyboardInputHandler.h"
#include "Io/Mouse.h"

#include "Utility/String/Format.h"

#include "CoopJoinState.h"

enum class CoopMenuField {
    COOP_MENU_FIELD_NAME = 0,
    COOP_MENU_FIELD_ADDRESS = 1,
    COOP_MENU_FIELD_PORT = 2,
    COOP_MENU_FIELD_PARTY = 3,
};
using enum CoopMenuField;

/**
 * What the co-op menu shows: a choice between hosting and joining, or the form for one of them.
 */
enum class CoopMenuPage {
    COOP_MENU_PAGE_CHOOSE,
    COOP_MENU_PAGE_HOST,
    COOP_MENU_PAGE_JOIN,
};
using enum CoopMenuPage;

static constexpr int FIELD_COUNT = 4;
static constexpr int FIELD_MAX_LENGTH = 40;
static constexpr std::array<const char *, FIELD_COUNT> FIELD_LABELS = {"Your name", "Host address", "Port", "Your party"};
static constexpr char HOST_PARTY_COPY_TEXT[] = "Copy of the host's party";

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
    static constexpr std::array<int, 4> JOIN_FIELDS = {std::to_underlying(COOP_MENU_FIELD_NAME), std::to_underlying(COOP_MENU_FIELD_ADDRESS),
                                                       std::to_underlying(COOP_MENU_FIELD_PORT), std::to_underlying(COOP_MENU_FIELD_PARTY)};
    switch (page) {
    case COOP_MENU_PAGE_HOST: return HOST_FIELDS;
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
        if (page == COOP_MENU_PAGE_JOIN) {
            partySaves = loadMenuSlots();
            std::erase_if(partySaves, [](const SavegameSlot &slot) { return slot.fileName == COOP_JOINED_SAVE_FILE_NAME; });
            if (!std::ranges::contains(partySaves, fields[std::to_underlying(COOP_MENU_FIELD_PARTY)], &SavegameSlot::fileName))
                fields[std::to_underlying(COOP_MENU_FIELD_PARTY)].clear();
        }
        if (page == COOP_MENU_PAGE_CHOOSE) {
            CreateButton("CoopMenu_HostPage", {PANEL_X + 20, rowY(0)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_ShowHostPage, 0);
            CreateButton("CoopMenu_JoinPage", {PANEL_X + 20, rowY(1)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_ShowJoinPage, 0);
            CreateButton("CoopMenu_Back", {PANEL_X + 20, rowY(2)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Back, 0, INPUT_ACTION_ESCAPE);
            return;
        }

        std::span<const int> fields = pageFields(page);
        for (size_t i = 0; i < fields.size(); i++)
            CreateButton(fmt::format("CoopMenu_Field{}", fields[i]), {PANEL_X, rowY(i)}, {PANEL_W, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_SelectField, fields[i]);
        if (page == COOP_MENU_PAGE_HOST) {
            CreateButton("CoopMenu_Host", {PANEL_X + 20, buttonsY()}, {150, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Host, 0);
        } else {
            CreateButton("CoopMenu_Join", {PANEL_X + 20, buttonsY()}, {150, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopMenu_Join, 0);
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

        switch (page) {
        case COOP_MENU_PAGE_CHOOSE:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Multiplayer", frameRect);
            drawButton("CoopMenu_HostPage", "Host a game");
            drawButton("CoopMenu_JoinPage", "Join a friend's game");
            drawButton("CoopMenu_Back", "Back");
            DrawText(font, {PANEL_X, buttonsY()}, colorTable.Gray, "One player hosts, the others join.", frameRect);
            break;
        case COOP_MENU_PAGE_HOST:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Host a game", frameRect);
            drawFields(font);
            drawButton("CoopMenu_Host", "Start hosting");
            drawButton("CoopMenu_Back", "Back");
            DrawText(font, {PANEL_X, buttonsY() + ROW_H}, colorTable.Gray, "Start or load a game next. Friends join with", frameRect);
            DrawText(font, {PANEL_X, buttonsY() + ROW_H + 20}, colorTable.Gray, "your IP address, the port must be open to UDP.", frameRect);
            break;
        case COOP_MENU_PAGE_JOIN:
            DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Join a friend's game", frameRect);
            drawFields(font);
            drawButton("CoopMenu_Join", "Join");
            drawButton("CoopMenu_Back", "Back");
            DrawText(font, {PANEL_X, buttonsY() + ROW_H}, colorTable.Gray, "Click Your party to bring one of your saves.", frameRect);
            DrawText(font, {PANEL_X, buttonsY() + ROW_H + 20}, colorTable.Gray, "Saving as usual keeps your progress in it.", frameRect);
            break;
        }

        if (!error.empty())
            DrawText(font, {PANEL_X, buttonsY() + 3 * ROW_H}, colorTable.Red, error, frameRect);
    }

    /**
     * Selects the next of the guest's saves to bring a party from, after the last one a copy of the host's party.
     */
    void chooseNextPartySave() {
        std::string &party = fields[std::to_underlying(COOP_MENU_FIELD_PARTY)];
        auto pos = std::ranges::find(partySaves, party, &SavegameSlot::fileName);
        if (pos == partySaves.end()) {
            party = partySaves.empty() ? std::string() : partySaves.front().fileName;
        } else {
            ++pos;
            party = pos == partySaves.end() ? std::string() : pos->fileName;
        }
    }

    std::array<std::string, FIELD_COUNT> fields = {"Player", "", fmt::format("{}", COOP_DEFAULT_PORT), ""}; // The party is a file name in `saves`.
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
            std::string value = field == std::to_underlying(COOP_MENU_FIELD_PARTY) ? partyName() : fields[field] + (field == selected ? "_" : "");
            DrawText(font, {VALUE_X, rowY(i)}, color, value, frameRect);
        }
    }

    std::string partyName() const {
        const std::string &party = fields[std::to_underlying(COOP_MENU_FIELD_PARTY)];
        auto pos = std::ranges::find(partySaves, party, &SavegameSlot::fileName);
        return pos == partySaves.end() ? HOST_PARTY_COPY_TEXT : pos->header.name;
    }

    GUIButton *findButton(std::string_view id) {
        auto pos = std::ranges::find_if(vButtons, [&](GUIButton *button) { return button->id == id; });
        return pos == vButtons.end() ? nullptr : *pos;
    }

    static int rowY(int row) {
        return PANEL_Y + row * ROW_H;
    }

    int buttonsY() const {
        int rows = page == COOP_MENU_PAGE_CHOOSE ? 3 : pageFields(page).size(); // The choice page has three buttons and no fields.
        return PANEL_Y + rows * ROW_H + 15;
    }

    std::vector<SavegameSlot> partySaves;
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
            int next = shown[(pos - shown.begin() + 1) % shown.size()];
            if (next == std::to_underlying(COOP_MENU_FIELD_PARTY))
                next = shown.front(); // The party is picked by clicking, not typed.
            selectField(next);
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
        case UIMSG_CoopMenu_ShowHostPage:
            showPage(COOP_MENU_PAGE_HOST);
            break;
        case UIMSG_CoopMenu_ShowJoinPage:
            showPage(COOP_MENU_PAGE_JOIN);
            break;
        case UIMSG_CoopMenu_SelectField:
            if (param == std::to_underlying(COOP_MENU_FIELD_PARTY)) {
                _window->chooseNextPartySave();
            } else {
                selectField(param);
            }
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
    if (page == COOP_MENU_PAGE_HOST)
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
        coopSession = CoopSession::join(address, *port, _window->fields[std::to_underlying(COOP_MENU_FIELD_NAME)], std::move(guestSave), party);
    } catch (const std::exception &e) {
        _window->error = e.what();
        return false;
    }
    return true;
}
