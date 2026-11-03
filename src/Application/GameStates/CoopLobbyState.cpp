#include "CoopLobbyState.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AssetsManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineGlobals.h"
#include "Engine/Coop/CoopClient.h"
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

#include "Io/Mouse.h"

#include "Utility/String/Format.h"

#include "CoopJoinState.h"

static constexpr int PANEL_X = 120;
static constexpr int PANEL_Y = 110;
static constexpr int PANEL_W = 400;
static constexpr int ROW_H = 26;
static constexpr int VALUE_X = PANEL_X + 200;
static constexpr int MAX_ROWS = 4; // The host and three guests.

/**
 * Window of the lobby, it draws who's there and owns the buttons.
 */
class GUIWindow_CoopLobby : public GUIWindow {
 public:
    GUIWindow_CoopLobby() : GUIWindow(WINDOW_COOP_LOBBY, {0, 0}, render->GetRenderDimensions()) {
        _background = assets->getIcon("mm6title.pcx");
        if (coopSession && coopSession->server())
            CreateButton("CoopLobby_Start", {PANEL_X + 20, buttonsY()}, {150, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopLobby_Start, 0);
        CreateButton("CoopLobby_Back", {PANEL_X + 280, buttonsY()}, {100, ROW_H}, BUTTON_TYPE_NORMAL, 0, UIMSG_CoopLobby_Back, 0, INPUT_ACTION_ESCAPE);
    }

    virtual ~GUIWindow_CoopLobby() {
        assets->releaseImage(_background);
    }

    virtual void Update() override {
        render->DrawQuad2D(_background, {0, 0});
        DrawPopupWindow(PANEL_X - 20, PANEL_Y - 50, PANEL_W + 40, buttonsY() - PANEL_Y + 135);

        GUIFont *font = assets->pFontCreate.get();
        Pointi mousePos = mouse->position();
        bool isHost = coopSession && coopSession->server();
        DrawText(font, {PANEL_X, PANEL_Y - 35}, colorTable.White, isHost ? "New game together" : "Waiting for the host", frameRect);

        if (!failure.empty()) {
            DrawText(font, {PANEL_X, rowY(0)}, colorTable.Red, failure, frameRect);
        } else {
            std::vector<CoopLobbyPlayer> players = lobbyPlayers();
            for (int row = 0; row < std::min<int>(players.size(), MAX_ROWS); row++) {
                DrawText(font, {PANEL_X, rowY(row)}, colorTable.White, players[row].name, frameRect);
                if (players[row].ready) {
                    DrawText(font, {VALUE_X, rowY(row)}, colorTable.LaserLemon, "Ready", frameRect);
                } else {
                    DrawText(font, {VALUE_X, rowY(row)}, colorTable.Gray, "Making a party", frameRect);
                }
            }
        }

        for (GUIButton *button : vButtons) {
            Color color = button->Contains(mousePos) ? colorTable.LaserLemon : colorTable.White;
            DrawText(font, button->rect.topLeft(), color, button->msg == UIMSG_CoopLobby_Start ? "Start" : "Back", frameRect);
        }

        std::string_view hint = isHost ? "Start once everyone is ready. Friends join" : "The game starts when the host says so.";
        DrawText(font, {PANEL_X, buttonsY() + ROW_H + 5}, colorTable.Gray, hint, frameRect);
        if (isHost)
            DrawText(font, {PANEL_X, buttonsY() + ROW_H + 25}, colorTable.Gray, "with your IP address from Multiplayer.", frameRect);
        if (!error.empty())
            DrawText(font, {PANEL_X, buttonsY() + ROW_H + 50}, colorTable.Red, error, frameRect);
    }

    std::string error;
    std::string failure; // Why the guest's session ended, nothing else is shown then.

 private:
    static std::vector<CoopLobbyPlayer> lobbyPlayers() {
        if (!coopSession)
            return {};
        if (coopSession->server())
            return coopSession->server()->lobbyPlayers();
        return coopSession->client()->lobbyPlayers();
    }

    static int rowY(int row) {
        return PANEL_Y + row * ROW_H;
    }

    static int buttonsY() {
        return PANEL_Y + MAX_ROWS * ROW_H + 15;
    }

    GraphicsImage *_background = nullptr;
};

CoopLobbyState::CoopLobbyState() = default;
CoopLobbyState::~CoopLobbyState() = default;

FsmAction CoopLobbyState::enter() {
    _window = std::make_unique<GUIWindow_CoopLobby>();
    return FsmAction::none();
}

FsmAction CoopLobbyState::update() {
    if (!coopSession)
        return FsmAction::transition("back"); // Dropped from elsewhere, e.g. the game is shutting down.

    if (CoopClient *client = coopSession->client()) {
        if (client->state() == COOP_CLIENT_JOINED) {
            std::string fileName(COOP_JOINED_SAVE_FILE_NAME);
            ufs->write(fmt::format("saves/{}", fileName), coopSession->takeJoinedSave());
            engine->_pendingLoadFileName = fileName;
            SetCurrentMenuID(MENU_LoadingProcInMainMenu);
            return FsmAction::transition("joined");
        }
        if (client->state() == COOP_CLIENT_FAILED)
            _window->failure = fmt::format("Lost the host. {}.", client->failureReason());
    }

    while (engine->_messageQueue->haveMessages()) {
        UIMessageType message;
        engine->_messageQueue->popMessage(&message, nullptr, nullptr);
        switch (message) {
        case UIMSG_CoopLobby_Start: {
            std::vector<CoopLobbyPlayer> players = coopSession->server()->lobbyPlayers();
            if (!std::ranges::all_of(players, &CoopLobbyPlayer::ready)) {
                _window->error = "Wait until everyone's party is ready";
                break;
            }
            coopSession->server()->startGame();
            SetCurrentMenuID(MENU_COOP_START_NEW_GAME);
            return FsmAction::transition("startGame");
        }
        case UIMSG_CoopLobby_Back:
        case UIMSG_Escape:
            coopSession.reset();
            return FsmAction::transition("back");
        default:
            break;
        }
    }
    return FsmAction::none();
}

void CoopLobbyState::exit() {
    _window.reset();
}
