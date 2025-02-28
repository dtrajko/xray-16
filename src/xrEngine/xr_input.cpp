#include "stdafx.h"
#pragma hdrstop

#include "xr_input.h"
#include "IInputReceiver.h"
#include "GameFont.h"
#include "xrCore/Text/StringConversion.hpp"
#include "xrCore/xr_token.h"

CInput* pInput = nullptr;
IInputReceiver dummyController;

ENGINE_API float psMouseSens = 1.f;
ENGINE_API float psMouseSensScale = 1.f;
ENGINE_API Flags32 psMouseInvert = {false};

ENGINE_API float psControllerSens = 1.f;
ENGINE_API float psControllerDeadZoneSens = 0.f;

// Max events per frame
constexpr size_t MAX_KEYBOARD_EVENTS = 64;
constexpr size_t MAX_MOUSE_EVENTS = 256;
constexpr size_t MAX_CONTROLLER_EVENTS = 64;

CInput::CInput(const bool exclusive)
{
    exclusiveInput = exclusive;

    Log("Starting INPUT device...");

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0)
    {
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            OpenController(i);
    }

    m_mouseDelta = 25;

    mouseState.reset();
    keyboardState.reset();
    controllerState.reset();
    ZeroMemory(mouseTimeStamp, sizeof(mouseTimeStamp));
    ZeroMemory(controllerAxisState, sizeof(controllerAxisState));
    last_input_controller = -1;

    //===================== Dummy pack
    iCapture(&dummyController);

    SDL_StopTextInput(); // sanity

    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this, REG_PRIORITY_HIGH);
    Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
}

CInput::~CInput()
{
    GrabInput(false);

    for (auto& controller : controllers)
        SDL_GameControllerClose(controller);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    Device.seqFrame.Remove(this);
    Device.seqAppDeactivate.Remove(this);
    Device.seqAppActivate.Remove(this);
}

void CInput::OpenController(int idx)
{
    if (!SDL_IsGameController(idx))
        return;

    const auto controller = SDL_GameControllerOpen(idx);
    if (!controller)
        return;

    controllers.emplace_back(controller);
}

//-----------------------------------------------------------------------

void CInput::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    font.OutNext("*** INPUT:    %2.2fms", GetStats().FrameTime.result);
}

void CInput::MouseUpdate()
{
    // Mouse2 is a middle button in SDL,
    // but in X-Ray this is a right button
    constexpr int MouseButtonToKey[] = { MOUSE_1, MOUSE_3, MOUSE_2, MOUSE_4, MOUSE_5 };

    bool mouseMoved = false;
    int offs[COUNT_MOUSE_AXIS]{};
    const auto mousePrev = mouseState;

    SDL_Event events[MAX_MOUSE_EVENTS];
    SDL_PumpEvents();
    const auto count = SDL_PeepEvents(events, MAX_MOUSE_EVENTS,
        SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEWHEEL);

    for (int i = 0; i < count; ++i)
    {
        const SDL_Event event = events[i];

        switch (event.type)
        {
        case SDL_MOUSEMOTION:
            mouseMoved = true;
            mouseTimeStamp[0] = m_curTime + event.motion.timestamp;
            mouseTimeStamp[1] = m_curTime + event.motion.timestamp;
            offs[0] += event.motion.xrel;
            offs[1] += event.motion.yrel;
            break;

        case SDL_MOUSEBUTTONDOWN:
            mouseState[event.button.button - 1] = true;
            cbStack.back()->IR_OnMousePress(MouseButtonToKey[event.button.button - 1]);
            break;

        case SDL_MOUSEBUTTONUP:
            mouseState[event.button.button - 1] = false;
            cbStack.back()->IR_OnMouseRelease(MouseButtonToKey[event.button.button - 1]);
            break;

        case SDL_MOUSEWHEEL:
            mouseMoved = true;
            mouseTimeStamp[2] = m_curTime + event.wheel.timestamp;
            mouseTimeStamp[3] = m_curTime + event.wheel.timestamp;
            offs[2] += event.wheel.y;
            offs[3] += event.wheel.x;
            break;
        }
    }

    for (int i = 0; i < MOUSE_COUNT; ++i)
    {
        if (mouseState[i] && mousePrev[i])
            cbStack.back()->IR_OnMouseHold(MouseButtonToKey[i]);
    }

    if (mouseMoved)
    {
        if (offs[0] || offs[1])
            cbStack.back()->IR_OnMouseMove(offs[0], offs[1]);
        if (offs[2] || offs[3])
            cbStack.back()->IR_OnMouseWheel(offs[2], offs[3]);
    }
    else
    {
        if (mouseTimeStamp[1] && m_curTime - mouseTimeStamp[1] >= m_mouseDelta)
            cbStack.back()->IR_OnMouseStop(0, mouseTimeStamp[1] = 0);
        if (mouseTimeStamp[0] && m_curTime - mouseTimeStamp[0] >= m_mouseDelta)
            cbStack.back()->IR_OnMouseStop(0, mouseTimeStamp[0] = 0);
    }
}

void CInput::KeyUpdate()
{
    SDL_Event events[MAX_KEYBOARD_EVENTS];
    const auto count = SDL_PeepEvents(events, MAX_KEYBOARD_EVENTS,
        SDL_GETEVENT, SDL_KEYDOWN, SDL_TEXTINPUT);

    for (int i = 0; i < count; ++i)
    {
        const SDL_Event event = events[i];

        switch (event.type)
        {
        case SDL_KEYDOWN:
            if (event.key.repeat)
                continue;
            keyboardState[event.key.keysym.scancode] = true;
            cbStack.back()->IR_OnKeyboardPress(event.key.keysym.scancode);
            break;

        case SDL_KEYUP:
            keyboardState[event.key.keysym.scancode] = false;
            cbStack.back()->IR_OnKeyboardRelease(event.key.keysym.scancode);
            break;

        case SDL_TEXTINPUT:
            cbStack.back()->IR_OnTextInput(event.text.text);
            break;

        default:
            // Nothing here
            break;
        }
    }

    for (u32 i = 0; i < COUNT_KB_BUTTONS; ++i)
        if (keyboardState[i])
            cbStack.back()->IR_OnKeyboardHold(i);
}

void CInput::ControllerUpdate()
{
    constexpr int ControllerButtonToKey[] =
    {
        XR_CONTROLLER_BUTTON_A,
        XR_CONTROLLER_BUTTON_B,
        XR_CONTROLLER_BUTTON_X,
        XR_CONTROLLER_BUTTON_Y,
        XR_CONTROLLER_BUTTON_BACK,
        XR_CONTROLLER_BUTTON_GUIDE,
        XR_CONTROLLER_BUTTON_START,
        XR_CONTROLLER_BUTTON_LEFTSTICK,
        XR_CONTROLLER_BUTTON_RIGHTSTICK,
        XR_CONTROLLER_BUTTON_LEFTSHOULDER,
        XR_CONTROLLER_BUTTON_RIGHTSHOULDER,
        XR_CONTROLLER_BUTTON_DPAD_UP,
        XR_CONTROLLER_BUTTON_DPAD_DOWN,
        XR_CONTROLLER_BUTTON_DPAD_LEFT,
        XR_CONTROLLER_BUTTON_DPAD_RIGHT,
        XR_CONTROLLER_BUTTON_MISC1,
        XR_CONTROLLER_BUTTON_PADDLE1,
        XR_CONTROLLER_BUTTON_PADDLE2,
        XR_CONTROLLER_BUTTON_PADDLE3,
        XR_CONTROLLER_BUTTON_PADDLE4,
        XR_CONTROLLER_BUTTON_TOUCHPAD,
    };

    SDL_Event events[MAX_CONTROLLER_EVENTS];
    auto count = SDL_PeepEvents(events, MAX_CONTROLLER_EVENTS,
        SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEADDED);

    for (int i = 0; i < count; ++i)
    {
        const SDL_Event& event = events[i];
        OpenController(event.cdevice.which);
    }

    if (!IsControllerAvailable())
        return;

    const int controllerDeadZone = int(psControllerDeadZoneSens * (SDL_JOYSTICK_AXIS_MAX / 100.f)); // raw

    const auto controllerPrev = controllerState;
    decltype(controllerAxisState) controllerAxisStatePrev;
    CopyMemory(controllerAxisStatePrev, controllerAxisState, sizeof(controllerAxisState));

    count = SDL_PeepEvents(events, MAX_CONTROLLER_EVENTS,
        SDL_GETEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERDEVICEREMOVED);

    for (int i = 0; i < count; ++i)
    {
        const SDL_Event& event = events[i];

        switch (event.type)
        {
        case SDL_CONTROLLERAXISMOTION:
        {
            if (event.caxis.axis >= COUNT_CONTROLLER_AXIS)
                break; // SDL added new axis, not supported by engine yet

            if (last_input_controller != event.caxis.which) // don't write if don't really need to
                last_input_controller = event.caxis.which;

            if (std::abs(event.caxis.value) < controllerDeadZone)
                controllerAxisState[event.caxis.axis] = 0;
            else
                controllerAxisState[event.caxis.axis] = event.caxis.value;
            break;
        }

        case SDL_CONTROLLERBUTTONDOWN:
            if (event.cbutton.button >= XR_CONTROLLER_BUTTON_COUNT)
                break; // SDL added new button, not supported by engine yet

            controllerState[event.cbutton.button] = true;
            cbStack.back()->IR_OnControllerPress(ControllerButtonToKey[event.cbutton.button], 1.f, 0.f);
            break;

        case SDL_CONTROLLERBUTTONUP:
            if (event.cbutton.button >= XR_CONTROLLER_BUTTON_COUNT)
                break; // SDL added new button, not supported by engine yet

            controllerState[event.cbutton.button] = false;
            cbStack.back()->IR_OnControllerRelease(ControllerButtonToKey[event.cbutton.button], 0.f, 0.f);
            break;

        case SDL_CONTROLLERDEVICEADDED:
            OpenController(event.cdevice.which);
            break;

        case SDL_CONTROLLERDEVICEREMOVED:
        {
            const auto controller = SDL_GameControllerFromInstanceID(event.cdevice.which);
            const auto it = std::find(controllers.begin(), controllers.end(), controller);
            if (it != controllers.end())
                controllers.erase(it);
            break;
        }
        } // switch (event.type)
    }

    for (int i = 0; i < COUNT_CONTROLLER_BUTTONS; ++i)
    {
        if (controllerState[i] && controllerPrev[i])
            cbStack.back()->IR_OnControllerHold(ControllerButtonToKey[i], 1.f, 0.f);
    }

    const auto checkAxis = [this](int axis, int rawX, int rawY, int prevRawX, int prevRawY)
    {
        const auto quantize = [](int value)
        {
            return value / (SDL_JOYSTICK_AXIS_MAX / 100.f);
        };

        const auto x = quantize(rawX), y = quantize(rawY), prevX = quantize(prevRawX), prevY = quantize(prevRawY);
        const bool xActive = !fis_zero(x), yActive = !fis_zero(y), prevXActive = !fis_zero(prevX), prevYActive = !fis_zero(prevY);

        if ((xActive && prevXActive) || (yActive && prevYActive))
            cbStack.back()->IR_OnControllerHold(axis, x, y);
        else if (xActive || yActive)
            cbStack.back()->IR_OnControllerPress(axis, x, y);
        else if (prevXActive || prevYActive)
            cbStack.back()->IR_OnControllerRelease(axis, 0.f, 0.f);
    };

    checkAxis(XR_CONTROLLER_AXIS_LEFT,          controllerAxisState[0], controllerAxisState[1], controllerAxisStatePrev[0], controllerAxisStatePrev[1]);
    checkAxis(XR_CONTROLLER_AXIS_RIGHT,         controllerAxisState[2], controllerAxisState[3], controllerAxisStatePrev[2], controllerAxisStatePrev[3]);
    checkAxis(XR_CONTROLLER_AXIS_TRIGGER_LEFT,  controllerAxisState[4], 0,                      controllerAxisStatePrev[4], 0);
    checkAxis(XR_CONTROLLER_AXIS_TRIGGER_RIGHT, controllerAxisState[5], 0,                      controllerAxisStatePrev[5], 0);
}

bool KbdKeyToButtonName(const int dik, xr_string& name)
{
    static std::locale locale("");

    if (dik >= 0)
    {
        name = StringFromUTF8(SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)dik)), locale);
        return true;
    }

    return false;
}

bool OtherDevicesKeyToButtonName(const int btn, xr_string& name)
{
    if (btn > CInput::COUNT_KB_BUTTONS)
    {
        // XXX: Not implemented
        return false; // true;
    }

    return false;
}

bool CInput::GetKeyName(const int dik, pstr dest_str, int dest_sz)
{
    xr_string keyname;
    bool result;

    if (dik < COUNT_KB_BUTTONS)
        result = KbdKeyToButtonName(dik, keyname);
    else
        result = OtherDevicesKeyToButtonName(dik, keyname);

    if (keyname.empty())
        return false;

    xr_strcpy(dest_str, dest_sz, keyname.c_str());
    return result;
}

bool CInput::iGetAsyncKeyState(const int dik)
{
    if (dik < COUNT_KB_BUTTONS)
        return keyboardState[dik];

    if (dik > MOUSE_INVALID && dik < MOUSE_MAX)
    {
        const int mk = dik - (MOUSE_INVALID + 1);
        return iGetAsyncBtnState(mk);
    }

    if (dik > XR_CONTROLLER_BUTTON_INVALID && dik < XR_CONTROLLER_BUTTON_MAX)
    {
        const int mk = dik - (XR_CONTROLLER_BUTTON_INVALID + 1);
        return iGetAsyncGpadBtnState(mk);
    }

    // unknown key ???
    return false;
}

bool CInput::iGetAsyncBtnState(const int btn)
{
    return mouseState[btn];
}

bool CInput::iGetAsyncGpadBtnState(const int btn)
{
    return controllerState[btn];
}

void CInput::iGetAsyncMousePos(Ivector2& p) const
{
    SDL_GetMouseState(&p.x, &p.y);
}

void CInput::iSetMousePos(const Ivector2& p) const
{
    SDL_WarpMouseInWindow(Device.m_sdlWnd, p.x, p.y);
}

void CInput::GrabInput(const bool grab)
{
    // Self descriptive
    SDL_ShowCursor(grab ? SDL_FALSE : SDL_TRUE);

    // Clip cursor to the current window
    // If SDL_HINT_GRAB_KEYBOARD is set then the keyboard will be grabbed too
    SDL_SetWindowGrab(Device.m_sdlWnd, grab ? SDL_TRUE : SDL_FALSE);

    // Grab the mouse
    if (exclusiveInput)
        SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);

    // We're done here.
    inputGrabbed = grab;
}

bool CInput::InputIsGrabbed() const
{
    return inputGrabbed;
}

void CInput::iCapture(IInputReceiver* p)
{
    VERIFY(p);

    // change focus
    if (!cbStack.empty())
        cbStack.back()->IR_OnDeactivate();
    cbStack.push_back(p);
    cbStack.back()->IR_OnActivate();

    // prepare for _new_ controller
    ZeroMemory(mouseTimeStamp, sizeof(mouseTimeStamp));
    ZeroMemory(controllerAxisState, sizeof(controllerAxisState));
    last_input_controller = -1;
}

void CInput::iRelease(IInputReceiver* p)
{
    if (p == cbStack.back())
    {
        cbStack.back()->IR_OnDeactivate();
        cbStack.pop_back();
        cbStack.back()->IR_OnActivate();
    }
    else
    {
        // we are not topmost receiver, so remove the nearest one
        for (size_t cnt = cbStack.size(); cnt > 0; --cnt)
            if (cbStack[cnt - 1] == p)
            {
                xr_vector<IInputReceiver*>::iterator it = cbStack.begin();
                std::advance(it, cnt - 1);
                cbStack.erase(it);
                break;
            }
    }
}

void CInput::OnAppActivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnActivate();

    mouseState.reset();
    keyboardState.reset();
    controllerState.reset();
    ZeroMemory(mouseTimeStamp, sizeof(mouseTimeStamp));
    ZeroMemory(controllerAxisState, sizeof(controllerAxisState));
    last_input_controller = -1;
}

void CInput::OnAppDeactivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnDeactivate();

    mouseState.reset();
    keyboardState.reset();
    controllerState.reset();
    ZeroMemory(mouseTimeStamp, sizeof(mouseTimeStamp));
    ZeroMemory(controllerAxisState, sizeof(controllerAxisState));
    last_input_controller = -1;
}

void CInput::OnFrame(void)
{
    stats.FrameStart();
    stats.FrameTime.Begin();
    m_curTime = RDEVICE.TimerAsync_MMT();

    if (Device.dwPrecacheFrame == 0 && !Device.IsAnselActive)
    {
        KeyUpdate();
        MouseUpdate();
        ControllerUpdate();
    }

    stats.FrameTime.End();
    stats.FrameEnd();
}

IInputReceiver* CInput::CurrentIR()
{
    if (cbStack.size())
        return cbStack.back();

    return nullptr;
}

void CInput::ExclusiveMode(const bool exclusive)
{
    GrabInput(false);

    // Original CInput was using DirectInput in exclusive mode
    // In which keyboard was grabbed with the mouse.
    // Uncomment it below, if you want.
    //SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, exclusive ? "1" : "0");
    exclusiveInput = exclusive;

    GrabInput(true);
}

bool CInput::IsExclusiveMode() const
{
    return exclusiveInput;
}

void CInput::Feedback(FeedbackType type, float s1, float s2, float duration)
{
#if SDL_VERSION_ATLEAST(2, 0, 9)
    const u16 s1_rumble = iFloor(u16(-1) * clampr(s1, 0.0f, 1.0f));
    const u16 s2_rumble = iFloor(u16(-1) * clampr(s2, 0.0f, 1.0f));
    const u32 duration_ms = duration < 0.f ? 0 : iFloor(duration * 1000.f);

    switch (type)
    {
    case FeedbackController:
    {
        for (SDL_GameController* controller : controllers)
            SDL_GameControllerRumble(controller, s1_rumble, s2_rumble, duration_ms);
        break;
    }

    case FeedbackTriggers:
    {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (last_input_controller != -1)
        {
            const auto controller = SDL_GameControllerFromInstanceID(last_input_controller);
            SDL_GameControllerRumbleTriggers(controller, s1_rumble, s2_rumble, duration_ms);
        }
        break;
#endif
    }

    default: NODEFAULT;
    }
#endif
}
