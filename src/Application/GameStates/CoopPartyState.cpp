#include "CoopPartyState.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AssetsManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineGlobals.h"
#include "Engine/Coop/CoopClient.h"
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

#include "Io/Mouse.h"

#include "Utility/String/Format.h"

#include "CoopJoinState.h"

static constexpr int PANEL_X = 120;
static constexpr int PANEL_Y = 110;
static constexpr int PANEL_W = 400;
static constexpr int ROW_H = 26;
static constexpr int ROWS_PER_PAGE = 6; // Saves shown at once, under the new party row.

/**
 * Window of the party choice, it draws the list and owns its buttons.
 */
class GUIWindow_CoopParty : public GUIWindow {
 public:
    GUIWindow_CoopParty() : GUIWindow(WINDOW_COOP_PARTY, {0, 0}, render->GetRenderDimensions()) {
        _background = assets->getIcon("mm6title.pcx");
        saves = loadMenuSlots();
        std::erase_if(saves, [](const SavegameSlot &slot) { return slot.fileName == COOP_JOINED_SAVE_FILE_NAME; });
        showPage(0);
    }

    virtual ~GUIWindow_CoopParty() {
        assets->releaseImage(_background);
    }

    void showPage(int newPage) {
        page = newPage;
        DeleteButtons();
        CreateButton("CoopParty_New", {PANEL_X + 20, rowY(0)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopParty_New, 0);
        for (int row = 0; row < ROWS_PER_PAGE; row++) {
            int index = page * ROWS_PER_PAGE + row;
            if (index >= std::ssize(saves))
                break;
            CreateButton(fmt::format("CoopParty_Save{}", index), {PANEL_X + 20, rowY(row + 1)}, {PANEL_W - 40, ROW_H}, BUTTON_TYPE_NORMAL, 0,
                         UIMSG_CoopParty_Pick, index);
        }
        if (std::ssize(saves) > ROWS_PER_PAGE)
            CreateButton("CoopParty_More", {PANEL_X + 20, buttonsY()}, {150, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopParty_More, 0);
        CreateButton("CoopParty_Back", {PANEL_X + 280, buttonsY()}, {100, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopParty_Back, 0, INPUT_ACTION_ESCAPE);
    }

    virtual void Update() override {
        render->DrawQuad2D(_background, {0, 0});
        DrawPopupWindow(PANEL_X - 20, PANEL_Y - 50, PANEL_W + 40, buttonsY() - PANEL_Y + 135);

        GUIFont *font = assets->pFontCreate.get();
        Pointi mousePos = mouse->position();
        DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, "Your party", frameRect);
        for (GUIButton *button : vButtons) {
            std::string text;
            if (button->msg == UIMSG_CoopParty_New) {
                text = "Create a new party";
            } else if (button->msg == UIMSG_CoopParty_Pick) {
                text = saves[button->msg_param].header.name;
            } else if (button->msg == UIMSG_CoopParty_More) {
                text = "More saves";
            } else {
                text = "Back";
            }
            Color color = button->Contains(mousePos) ? colorTable.LaserLemon : colorTable.White;
            DrawText(font, button->rect.topLeft(), color, text, frameRect);
        }

        bool inLobby = coopSession && coopSession->client() && coopSession->client()->state() == COOP_CLIENT_IN_LOBBY;
        std::string_view hint = inLobby ? "The host is starting a new game." : "The host is already playing.";
        DrawText(font, {PANEL_X, buttonsY() + ROW_H + 5}, colorTable.Gray, hint, frameRect);
        DrawText(font, {PANEL_X, buttonsY() + ROW_H + 25}, colorTable.Gray, "Make a party, or bring one from a save.", frameRect);
    }

    std::vector<SavegameSlot> saves;
    int page = 0;

 private:
    static int rowY(int row) {
        return PANEL_Y + row * ROW_H;
    }

    static int buttonsY() {
        return PANEL_Y + (ROWS_PER_PAGE + 1) * ROW_H + 10;
    }

    GraphicsImage *_background = nullptr;
};

CoopPartyState::CoopPartyState() = default;
CoopPartyState::~CoopPartyState() = default;

FsmAction CoopPartyState::enter() {
    _window = std::make_unique<GUIWindow_CoopParty>();
    return FsmAction::none();
}

FsmAction CoopPartyState::update() {
    if (!coopSession || !coopSession->client())
        return FsmAction::transition("back"); // Dropped from elsewhere, e.g. the game is shutting down.
    if (coopSession->client()->state() == COOP_CLIENT_FAILED)
        return FsmAction::transition("lobby"); // The lobby tells why.

    while (engine->_messageQueue->haveMessages()) {
        UIMessageType message;
        int param = 0;
        engine->_messageQueue->popMessage(&message, &param, nullptr);
        switch (message) {
        case UIMSG_CoopParty_New:
            SetCurrentMenuID(MENU_NEWGAME);
            return FsmAction::transition("newParty");
        case UIMSG_CoopParty_Pick: {
            const std::string &fileName = _window->saves[param].fileName;
            coopSession->setGuestSave(Blob::copy(ufs->read(fmt::format("saves/{}", fileName))), fileName);
            coopSession->client()->setReady(true);
            return FsmAction::transition("lobby");
        }
        case UIMSG_CoopParty_More: {
            int pages = (std::ssize(_window->saves) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
            _window->showPage((_window->page + 1) % pages);
            break;
        }
        case UIMSG_CoopParty_Back:
        case UIMSG_Escape:
            coopSession.reset();
            return FsmAction::transition("back");
        default:
            break;
        }
    }
    return FsmAction::none();
}

void CoopPartyState::exit() {
    _window.reset();
}
