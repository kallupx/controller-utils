"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const filterSource =
    fs.readFileSync(path.join(__dirname, "../src/steamvr/input_filter.js"), "utf8");
const dispatchSource = require("./fixtures/steam_input_dispatch.json");
const systemSource = require("./fixtures/steam_system_input.json");
const serial = "TEST000000001", owner = "test-123";

function harness() {
    let time = 0, id = 0;
    const timers = new Map(), delivered = [], sources = [];
    const keyboard = [];
    const keyboardModes = [], windowStreams = [];
    let keyboardInput, keyboardComponent;
    const controllers = [ {
        nControllerIndex : 2,
        strSerialNumber : serial,
        eControllerType : 10,
        bBluetooth : true,
        bRemoteDevice : false
    } ];
    const context = vm.createContext({
        performance : {now : () => time},
        setTimeout : (fn, delay) => {
            timers.set(++id, {fn, at : time + delay});
            return id;
        },
        clearTimeout : token => timers.delete(token),
        ControllerStore : {GetControllers : () => controllers},
        SteamClient : {
            Input : {SetKeyboardActionset : (...args) => keyboardModes.push(args)},
            OpenVR : {Keyboard : {Show : () => assert.fail("Shared context has no overlay")}}
        },
        g_PopupManager : {
            GetPopupForVROverlayKey : key => {
                if (key === "valve.steam.gamepadui.keyboard")
                    return keyboardInput && {
                        window : {
                            SteamClient : {
                                Input : {
                                    EnableControllerAnalogInputMessages : enabled =>
                                        windowStreams.push(enabled)
                                }
                            },
                            document : {
                                hidden : false,
                                querySelectorAll :
                                    () => [{__reactFiber$test : {stateNode : keyboardComponent}}]
                            }
                        }
                    };
                assert.equal(key, "valve.steam.gamepadui.main");
                return {
                    window :
                        {SteamClient : {OpenVR : {Keyboard : {Show : () => keyboard.push("show")}}}}
                };
            }
        },
        he : () => {},
        oe : value => assert(value),
        n: {pR: {}},
        e: {Vz: {KEYBOARD_SIMULATOR: 3, GAMEPAD: 1}},
        w: {AE: (target, type, event) => delivered.push({type, ...event})},
        sourceChanged: (...args) => sources.push(args)
    });
    vm.runInContext(`
        globalThis.originalDispatch = ({${dispatchSource}}).OnButtonActionInternal;
        globalThis.FocusNavController = Object.assign(Object.create({OnButtonActionInternal: originalDispatch}), {
            m_rgCatchAllGamepadInput: [],
            m_rgGamepadInputSources: [Object.assign(Object.create({${systemSource}}), {
                HandleControllerInputMessages() {}, HandleSystemKeyEvents() {},
                SetSourceType() {},
                OnButtonDown(button,index) { FocusNavController.OnButtonActionInternal(true,button,1,index,false); },
                OnButtonUp(button,index) { FocusNavController.OnButtonActionInternal(false,button,1,index,false); },
                m_rgControllers: new Map([[2, {activeButtons: {}}]])})],
            GetEventTarget() { return [{}, null]; },
            ChangeNavigationSource(...args) { sourceChanged(...args); },
            SetSystemKeyAppIDHint(value) { this.m_nSystemKeyAppIDHint=value; },
            BatchedUpdate(fn) { return fn(); }
        });
        globalThis.filter = ${filterSource};
    `,
                    context);
    const nav = context.FocusNavController;
    nav.m_rgGamepadInputSources[0].m_NavController = nav;
    const renew = (enabled = true, changed = {}) =>
        context.filter({serial, owner, enabled, ...changed});
    const button = (down, code, source = 1, index = 2) => {
        // The real physical source updates this before calling the wrapped dispatch.
        const state = nav.m_rgGamepadInputSources[0].m_rgControllers.get(index);
        if (state)
            state.activeButtons[code] = down;
        return nav.OnButtonActionInternal(down, code, source, index, false);
    };
    const advance = milliseconds => {
        time += milliseconds;
        for (const [key, timer] of [...timers]) {
            if (timer.at <= time && timers.has(key)) {
                timers.delete(key);
                timer.fn();
            }
        }
    };
    const mountKeyboard = () => {
        keyboardInput = vm.runInContext(`new (class {
            analogRequests = [];
            EnableAnalogInputMessages(enabled) { this.analogRequests.push(enabled); }
            m_rgControllers = new Map([[2, {activeButtons: {}, controllerType: 10, controllerStyle: 0}]]);
            m_ButtonDownCallbacks = new (class { calls=[]; Dispatch(...args) { this.calls.push(args); } });
            m_ButtonUpCallbacks = new (class { calls=[]; Dispatch(...args) { this.calls.push(args); } });
            m_AnalogCallbacks = new (class { calls=[]; listeners=new Set();
                Dispatch(...args) { this.calls.push(args); for (const fn of this.listeners) fn(...args); } });
            RegisterForAnalog(fn) { this.m_AnalogCallbacks.listeners.add(fn);
                return {Unregister: () => this.m_AnalogCallbacks.listeners.delete(fn)}; }
        })()`,
                                        context);
        const target = {BIsElementValidForInput : () => true};
        const manager = {
            m_currentVirtualKeyboardRef : target,
            m_ActiveElementProps : {},
            m_bIsVirtualKeyboardModal : {Value : false},
            SetVirtualKeyboardHidden() {
                keyboard.push('hide');
                this.m_lastActiveVirtualKeyboardRef = this.m_currentVirtualKeyboardRef;
                this.m_currentVirtualKeyboardRef = null;
            },
            ShowVirtualKeyboard(ref, props, modal) {
                assert.equal(ref, target);
                assert.equal(modal, false);
                keyboard.push('reopen');
                this.m_currentVirtualKeyboardRef = ref;
            }
        };
        keyboardComponent = {
            m_trackpadInput : keyboardInput,
            props : {
                VirtualKeyboardManager : manager,
                windowInstance :
                    {IsStandaloneKeyboardWindow : () => false, IsDesktopOverlayWindow : () => false}
            }
        };
        return keyboardInput;
    };
    return {
        context,
        nav,
        controllers,
        delivered,
        sources,
        keyboard,
        keyboardModes,
        windowStreams,
        renew,
        button,
        advance,
        mountKeyboard,
        get keyboardComponent() {
            return keyboardComponent;
        },
        activeButtons : nav.m_rgGamepadInputSources[0].m_rgControllers.get(2).activeButtons
    };
}

{
    const h = harness(), source = h.nav.m_rgGamepadInputSources[0];
    const system = (enabled = true, tap = 0, changed = {}) =>
        h.renew(enabled, {systemOnly : true, tap, owner : 'system-owner', ...changed});
    assert.equal(system().active, true);
    source.OnSystemButtonPress(27, 2);
    assert.equal(h.delivered.length, 0);
    source.OnSystemButtonPress(28, 2);
    source.OnSystemButtonPress(27, 3);
    assert.equal(h.delivered.length, 4);
    h.delivered.length = 0;
    h.renew();
    system(true, 1);
    assert.deepEqual(h.delivered.map(e => e.type), [ 'vgp_onbuttondown', 'vgp_onbuttonup' ]);
    system(true, 1);
    assert.equal(h.delivered.length, 2);
    assert.throws(() => system(true, 1, {owner : 'other'}), /Another/);
    h.advance(1000);
    assert.equal(Object.hasOwn(source, 'OnSystemButtonPress'), false);
    system(true, 1);
    assert.equal(h.delivered.length, 2);
    system(true, 2);
    assert.equal(h.delivered.length, 4);
    system(false, 2);
    assert.equal(Object.hasOwn(source, 'OnSystemButtonPress'), false);
    assert.equal(Object.hasOwn(h.nav, 'OnButtonActionInternal'), false);
    source.OnSystemButtonPress(27, 2);
    assert.equal(h.delivered.length, 6);
}
{
    const h = harness(), source = h.nav.m_rgGamepadInputSources[0];
    h.renew(true, {systemOnly : true, tap : 0});
    h.controllers[0].strSerialNumber = 'OTHER';
    source.OnSystemButtonPress(27, 2);
    assert.equal(h.delivered.length, 2);
    assert.equal(Object.hasOwn(source, 'OnSystemButtonPress'), false);
}
{
    const h = harness(), source = h.nav.m_rgGamepadInputSources[0];
    Object.getPrototypeOf(source).OnSystemButtonPress = function changed() {};
    assert.throws(() => h.renew(true, {systemOnly : true, tap : 0}), /Unsupported/);
}

{
    const h = harness();
    for (const fields of [{systemOnly : 'true'}, {systemOnly : true, tap : -1},
                          {systemOnly : true, tap : 0x100000000}, {systemOnly : true, tap : true},
                          {systemOnly : true, keyboard : true, tap : 0}, {tap : 1}])
        assert.throws(() => h.renew(true, fields), /Invalid/);
}

{
    const h = harness(), input = h.mountKeyboard();
    h.renew();
    const typing = () => h.renew(false, {typing : true, touches : 3, held : false});
    assert.equal(typing().keyboardWatch, true);
    assert.equal(Object.hasOwn(input.m_AnalogCallbacks, 'Dispatch'), false);
    assert.equal(Object.hasOwn(h.nav, 'OnButtonActionInternal'), false);
    for (let i = 0; i < 15; ++i) {
        input.m_AnalogCallbacks.Dispatch(19, 2, .3, .4);
        input.m_AnalogCallbacks.Dispatch(21, 2, -.3, -.4);
        h.advance(200);
        typing();
    }
    assert.equal(h.keyboardModes.length, 1);
    h.renew(true, {typing : false, touches : 0, held : false});
    h.advance(100);
    assert.deepEqual(h.keyboard, []);
    assert.equal(input.m_AnalogCallbacks.listeners.size, 0);
}

{
    const h = harness(), input = h.mountKeyboard();
    h.renew();
    const typing = (touches = 3) => h.renew(false, {typing : true, touches, held : false});
    typing();
    for (let i = 0; i < 10; ++i) {
        h.advance(200);
        typing();
    }
    assert.equal(h.keyboardModes.length, 2);
    assert.deepEqual(h.windowStreams, [ true, true ]);
    assert.deepEqual(h.keyboard, []);
    typing(0);
    h.advance(200);
    typing(0);
    assert.deepEqual(h.keyboard, []);
    h.renew(true, {typing : false, touches : 0, held : true});
    assert.deepEqual(h.keyboard, []);
    assert.equal(input.m_AnalogCallbacks.listeners.size, 0);
}

{
    const h = harness(), input = h.mountKeyboard();
    h.renew();
    for (let i = 0; i < 10; ++i) {
        h.renew(false, {typing : true, touches : 1, held : false});
        h.advance(200);
    }
    h.renew(true, {typing : false, touches : 0, held : false});
    assert.deepEqual(h.keyboard, [ 'hide' ]);
    h.advance(80);
    assert.deepEqual(h.keyboard, [ 'hide', 'reopen' ]);
    h.advance(2000);
    assert.deepEqual(h.keyboard, [ 'hide', 'reopen' ]);
    assert.equal(input.m_AnalogCallbacks.listeners.size, 0);
}

{
    const h = harness(), input = h.mountKeyboard();
    h.renew(false, {typing : true, touches : 2, held : false});
    const watch = h.context.__controllerutilsKeyboardWatchV1;
    h.renew(false,
            {keyboard : true, actionOnly : true, serial : 'keyboard-shortcut', owner : 'shortcut'});
    assert.equal(h.context.__controllerutilsKeyboardWatchV1, watch);
    assert.deepEqual(h.keyboard, [ 'show' ]);
    h.keyboard.length = 0;
    h.advance(1001);
    assert.equal(input.m_AnalogCallbacks.listeners.size, 0);
    assert.equal(h.context.__controllerutilsKeyboardWatchV1, undefined);
    assert.deepEqual(h.keyboard, []);
    for (const changed
             of [{typing : 1}, {typing : true}, {touches : 4}, {touches : .5}, {held : 1}])
        assert.throws(() => h.renew(true, changed), /Invalid/);
}

{
    const h = harness(), first = h.mountKeyboard();
    h.renew(false, {typing : true, touches : 2, held : false});
    const second = h.mountKeyboard();
    h.renew(false, {typing : true, touches : 2, held : false});
    assert.equal(first.m_AnalogCallbacks.listeners.size, 0);
    assert.equal(second.m_AnalogCallbacks.listeners.size, 1);
    h.controllers.splice(0);
    h.renew(false, {typing : true, touches : 2, held : false});
    assert.equal(second.m_AnalogCallbacks.listeners.size, 0);
    assert.deepEqual(h.keyboard, []);
}

{
    const h = harness(), input = h.mountKeyboard();
    for (let i = 0; i < 10; ++i) {
        h.renew(false, {typing : true, touches : 3, held : false});
        input.m_AnalogCallbacks.Dispatch(19, 2, .2, .4);
        input.m_AnalogCallbacks.Dispatch(21, 7, .2, .4);
        h.advance(200);
    }
    h.renew(true, {typing : false, touches : 0, held : false});
    h.keyboardComponent.props.VirtualKeyboardManager.m_currentVirtualKeyboardRef = {};
    h.advance(80);
    assert.deepEqual(h.keyboard, [ 'hide' ]);
}

{
    const h = harness();
    h.mountKeyboard();
    delete h.keyboardComponent.props.VirtualKeyboardManager.ShowVirtualKeyboard;
    for (let i = 0; i < 10; ++i) {
        h.renew(false, {typing : true, touches : 1, held : false});
        h.advance(200);
    }
    h.renew(true, {typing : false, touches : 0, held : false});
    h.advance(80);
    assert.deepEqual(h.keyboard, []);
}

{
    const h = harness();
    h.renew();
    const lease = h.context.__controllerutilsInputFilterV1;
    h.advance(500);
    assert.equal(h.renew(false, {
                      keyboard : true,
                      actionOnly : true,
                      serial : 'keyboard-shortcut',
                      owner : 'separate-process'
                  }).active,
                 false);
    assert.deepEqual(h.keyboard, [ 'show' ]);
    assert.equal(h.context.__controllerutilsInputFilterV1, lease);
    h.button(true, 1);
    h.button(false, 1);
    assert.equal(h.delivered.length, 0);
    h.advance(501);
    assert.equal(Object.hasOwn(h.nav, 'OnButtonActionInternal'), false);
    for (const changed of [{actionOnly : 1}, {actionOnly : true},
                           {actionOnly : true, keyboard : true, enabled : true}])
        assert.throws(() => h.renew(false, changed), /Invalid/);
}

{
    const h = harness();
    delete h.context.ControllerStore;
    delete h.context.FocusNavController;
    assert.equal(h.renew(false, {keyboard : true, actionOnly : true}).active, false);
    assert.deepEqual(h.keyboard, [ 'show' ]);
}

{
    const h = harness();
    h.nav.m_ActiveContext = {
        m_activeWindow :
            {SteamClient : {OpenVR : {Keyboard : {Show : () => h.keyboard.push("focused")}}}}
    };
    assert.equal(h.renew(true, {keyboard : true}).active, true);
    assert.deepEqual(h.keyboard, [ "focused" ]);
    h.renew(false);
}

{
    const h = harness();
    h.context.vrGamepadInput = {
        FocusedVROverlayOwnedBySteam : true,
        m_lastOverlayInputFocusChangedMessage : {overlay_key : () => 'valve.steam.gamepadui.main'}
    };
    const result = h.renew(false, {keyboard : true, actionOnly : true});
    assert.equal(result.keyboardOverlay, undefined);
    assert.deepEqual(h.keyboard, [ 'show' ]);
}

{
    const h = harness();
    h.renew();
    const lease = h.context.__controllerutilsInputFilterV1;
    const target = 'gamescope.gamescope-0.window.62';
    h.context.vrGamepadInput = {
        FocusedVROverlayOwnedBySteam : false,
        m_lastOverlayInputFocusChangedMessage : {overlay_key : () => target}
    };
    const result = h.renew(
        false,
        {keyboard : true, actionOnly : true, serial : 'keyboard-shortcut', owner : 'shortcut'});
    assert.equal(result.keyboardOverlay, target);
    assert.equal(result.active, false);
    assert.deepEqual(h.keyboard, []);
    assert.equal(h.context.__controllerutilsInputFilterV1, lease);
    h.renew(false);
}

{
    for (const target of ['', 'x'.repeat(256), 'window\n', 'window\0', null]) {
        const h = harness();
        h.context.vrGamepadInput = {
            FocusedVROverlayOwnedBySteam : false,
            m_lastOverlayInputFocusChangedMessage : {overlay_key : () => target}
        };
        assert.throws(() => h.renew(false, {keyboard : true, actionOnly : true}),
                      /Focused window keyboard is unavailable/);
        assert.deepEqual(h.keyboard, []);
    }
}

{
    const h = harness(), input = h.mountKeyboard();
    const down = input.m_ButtonDownCallbacks, up = input.m_ButtonUpCallbacks,
          analog = input.m_AnalogCallbacks;
    const original = down.Dispatch;
    assert.equal(h.renew().active, true);
    assert.equal(up.calls.length, 2);
    input.m_rgControllers.get(2).activeButtons[19] = true;
    input.m_rgControllers.get(2).activeButtons[21] = true;
    down.Dispatch(19, 1, 2, false);
    down.Dispatch(21, 1, 2, false);
    down.Dispatch(8, 1, 2, false);
    analog.Dispatch(19, 2, 120, 80);
    analog.Dispatch(21, 2, 300, 80);
    assert.equal(down.calls.length, 0);
    assert.equal(analog.calls.length, 0);
    down.Dispatch(19, 1, 7, false);
    analog.Dispatch(19, 7, 50, 60);
    assert.equal(down.calls.length, 1);
    assert.equal(analog.calls.length, 1);
    assert.equal(h.renew(false).active, false);
    assert.deepEqual(Array.from(input.analogRequests), [ true ]);
    assert.equal(down.Dispatch, original);
    assert.deepEqual([...down.calls.slice(1) ].map(args => args[0]), [ 19, 21 ]);
    assert.deepEqual([...analog.calls.slice(1) ].map(args => Array.from(args)),
                     [ [ 19, 2, 120, 80 ], [ 21, 2, 300, 80 ] ]);
    assert.equal(input.m_rgControllers.get(2).activeButtons[19], true);
    assert.equal(input.m_rgControllers.get(2).activeButtons[21], true);
    input.m_rgControllers.get(2).activeButtons[19] = false;
    input.m_rgControllers.get(2).activeButtons[21] = false;
    assert.equal(h.renew().active, true);
    down.Dispatch(19, 1, 2, false);
    analog.Dispatch(19, 2, 140, 90);
    assert.equal(down.calls.length, 3);
    assert.equal(analog.calls.length, 3);
    h.advance(1001);
    assert.equal(down.Dispatch, original);
}

{
    const h = harness();
    h.renew();
    const first = h.mountKeyboard(), original = first.m_AnalogCallbacks.Dispatch;
    h.renew();
    assert.notEqual(first.m_AnalogCallbacks.Dispatch, original);
    const second = h.mountKeyboard();
    h.renew();
    assert.equal(first.m_AnalogCallbacks.Dispatch, original);
    assert.deepEqual(Array.from(first.analogRequests), []);
    assert.equal(Object.hasOwn(second.m_AnalogCallbacks, "Dispatch"), true);
    h.renew(false);
    assert.equal(Object.hasOwn(second.m_AnalogCallbacks, "Dispatch"), false);
}

{
    const h = harness();
    assert.equal(h.renew().active, true);
    assert.equal(h.renew(true, {keyboard : true}).active, true);
    assert.deepEqual(h.keyboard, [ "show" ]);
    assert.equal(h.renew().active, true);
    assert.deepEqual(h.keyboard, [ "show" ]);
    h.renew(false, {keyboard : true});
    assert.deepEqual(h.keyboard, [ "show", "show" ]);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), false);
    assert.throws(() => h.renew(true, {keyboard : "true"}), /Invalid/);
    h.context.g_PopupManager.GetPopupForVROverlayKey = () => undefined;
    assert.throws(() => h.renew(true, {keyboard : true}), /keyboard is unavailable/);
}

{
    const h = harness();
    assert.equal(h.renew().active, true);
    h.button(true, 1);
    h.button(false, 1);
    assert.equal(h.delivered.length, 0);
    assert.equal(h.activeButtons[1], false);
    assert.equal(h.sources.length, 0);
    h.button(true, 27);
    h.button(false, 27);
    h.button(true, 28);
    h.button(false, 28);
    h.button(true, 1, 2);
    h.button(true, 1, 1, 3);
    assert.equal(h.delivered.length, 6);
    assert.equal(h.renew(false).active, false);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), false);
    h.button(true, 1);
    h.button(false, 1);
    assert.equal(h.delivered.length, 8);
}
{
    const h = harness();
    h.button(true, 1);
    assert.equal(h.renew().active, false);
    h.button(false, 1);
    assert.deepEqual(h.delivered.map(x => x.type), [ "vgp_onbuttondown", "vgp_onbuttonup" ]);
    assert.equal(h.renew().active, true);
    h.advance(800);
    assert.equal(h.renew().active, true);
    h.advance(900);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), true);
    h.advance(100);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), false);
    assert.equal(h.context.__controllerutilsInputFilterV1, undefined);
}
{
    const h = harness();
    h.renew();
    h.controllers[0].strSerialNumber = "ANOTHER";
    h.button(true, 1);
    assert.equal(h.delivered.length, 1);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), false);
    assert.equal(h.renew().active, false);
}
{
    const h = harness();
    h.renew();
    h.controllers[0].nControllerIndex = 4;
    assert.equal(h.renew().active, false);
    h.nav.m_rgGamepadInputSources[0].m_rgControllers.set(4, {activeButtons : {}});
    assert.equal(h.renew().index, 4);
    h.controllers.length = 0;
    assert.equal(h.renew().active, false);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), false);
}
{
    const h = harness();
    h.renew();
    const other = () => "new owner";
    h.nav.OnButtonActionInternal = other;
    assert.equal(h.renew().active, false);
    assert.equal(h.nav.OnButtonActionInternal, other);
    assert.throws(() => h.renew(), /dispatch owner/);
}
{
    const h = harness();
    h.renew();
    assert.throws(() => h.renew(false, {owner : "other"}), /Another/);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), true);
    h.advance(1000);
    assert.equal(h.renew(true, {owner : "other"}).active, true);
}
{
    const h = harness();
    Object.getPrototypeOf(h.nav).OnButtonActionInternal = function changedVersion() {};
    assert.throws(() => h.renew(), /Unsupported/);
    assert.equal(Object.hasOwn(h.nav, "OnButtonActionInternal"), false);
}
console.log("steam input filter JS lifecycle tests passed");
