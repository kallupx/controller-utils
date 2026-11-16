// Evaluated only in Steam's SharedJSContext. No raw input-source state is changed.
(function controllerutilsInputFilter(request) {
    "use strict";
    const key = "__controllerutilsInputFilterV1";
    const name = "OnButtonActionInternal";
    const now = () => performance.now();
    const inactive = reason => ({active : false, index : -1, serial : request.serial, reason});
    if (!request || !/^[A-Za-z0-9_-]{1,64}$/.test(request.serial) ||
        !/^[a-z0-9-]{1,80}$/.test(request.owner) || typeof request.enabled !== "boolean" ||
        (request.keyboard !== undefined && typeof request.keyboard !== "boolean") ||
        (request.actionOnly !== undefined &&
         (typeof request.actionOnly !== "boolean" ||
          (request.actionOnly && (!request.keyboard || request.enabled)))) ||
        (request.typing !== undefined &&
         (typeof request.typing !== "boolean" || request.typing && request.enabled)) ||
        (request.touches !== undefined &&
         (!Number.isInteger(request.touches) || request.touches < 0 || request.touches > 3)) ||
        (request.held !== undefined && typeof request.held !== "boolean") ||
        (request.systemOnly !== undefined && typeof request.systemOnly !== "boolean") ||
        (request.systemOnly && (request.keyboard || request.actionOnly || request.typing)) ||
        (request.tap !== undefined && (!request.systemOnly || !Number.isInteger(request.tap) ||
                                       request.tap < 0 || request.tap > 0xffffffff)))
        throw new Error("Invalid controller input lease request");

    const controllerIndex = () => {
        const all = globalThis.ControllerStore?.GetControllers();
        if (!Array.isArray(all))
            return -1;
        const matches =
            all.filter(c => c.strSerialNumber === request.serial && c.eControllerType === 10 &&
                            c.bBluetooth === true && !c.bRemoteDevice);
        if (matches.length !== 1)
            return -1;
        const index = matches[0].nControllerIndex;
        return Number.isInteger(index) && index >= 0 && index < 16 &&
                       all.filter(c => c.nControllerIndex === index).length === 1
                   ? index
                   : -1;
    };
    const reviewed = (fn, length, expected) => {
        const text = Function.prototype.toString.call(fn);
        let hash = 2166136261;
        for (let i = 0; i < text.length; ++i)
            hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
        return text.length === length && hash === expected;
    };
    if (request.systemOnly) {
        const systemKey = "__controllerutilsSystemInputV1";
        let system = globalThis[systemKey];
        if (system?.active && (system.deadline <= now() || controllerIndex() !== system.index))
            system.close();
        if (system?.active && (system.owner !== request.owner || system.serial !== request.serial))
            throw new Error("Another controller owns Steam button input");
        if (!request.enabled) {
            if (system?.owner === request.owner)
                system.close();
            return inactive("");
        }
        const index = controllerIndex();
        if (index < 0)
            return inactive("Bluetooth Steam Controller is disconnected or ambiguous");
        const sources = globalThis.FocusNavController?.m_rgGamepadInputSources?.filter(
            source => typeof source.HandleSystemKeyEvents === "function" &&
                      typeof source.OnSystemButtonPress === "function");
        if (sources?.length !== 1)
            throw new Error("Steam system button input is unavailable");
        const source = sources[0];
        const nav = globalThis.FocusNavController;
        if (source.m_NavController !== nav || typeof nav.SetSystemKeyAppIDHint !== "function")
            throw new Error("Steam system button context changed");
        if (system?.active &&
            (source !== system.source || source.OnSystemButtonPress !== system.wrapper)) {
            system.close();
            return inactive("Steam system button dispatch changed");
        }
        if (!system?.active) {
            if (Object.hasOwn(source, "OnSystemButtonPress") ||
                !reviewed(source.OnSystemButtonPress, 184, 1945712728))
                throw new Error("Unsupported Steam system button dispatcher");
            const original = source.OnSystemButtonPress;
            const priorTap = system?.owner === request.owner ? system.tap : request.tap;
            let timer;
            system = {
                owner : request.owner,
                serial : request.serial,
                index,
                source,
                tap : priorTap,
                active : true,
                deadline : 0,
                close() {
                    clearTimeout(timer);
                    if (source.OnSystemButtonPress === this.wrapper)
                        delete source.OnSystemButtonPress;
                    this.active = false;
                },
                wrapper(button, controller) {
                    if (now() >= system.deadline || controllerIndex() !== index) {
                        system.close();
                        return original.apply(this, arguments);
                    }
                    if (button === 27 && controller === index) {
                        system.hint = nav.m_nSystemKeyAppIDHint;
                        return;
                    }
                    return original.apply(this, arguments);
                },
                renew() {
                    this.deadline = now() + 1000;
                    clearTimeout(timer);
                    timer = setTimeout(() => this.close(), 1000);
                },
                tapButton() {
                    const hint = nav.m_nSystemKeyAppIDHint;
                    nav.SetSystemKeyAppIDHint(this.hint);
                    try {
                        original.call(source, 27, index);
                    } finally {
                        nav.SetSystemKeyAppIDHint(hint);
                    }
                }
            };
            Object.defineProperty(source, "OnSystemButtonPress",
                                  {value : system.wrapper, writable : true, configurable : true});
            globalThis[systemKey] = system;
        }
        system.renew();
        if (request.tap > system.tap) {
            system.tap = request.tap;
            system.tapButton();
        }
        return {active : true, index, serial : request.serial, reason : ""};
    }
    let lease = globalThis[key];
    if (!request.actionOnly) {
        if (lease && lease.deadline <= now()) {
            lease.close();
            lease = undefined;
        }
        if (lease && (lease.owner !== request.owner || lease.serial !== request.serial))
            throw new Error("Another controller input filter owns this context");
    }
    if (request.keyboard) {
        const input = globalThis.vrGamepadInput;
        if (request.actionOnly && input?.FocusedVROverlayOwnedBySteam === false) {
            const focus = input.m_lastOverlayInputFocusChangedMessage;
            const overlay = typeof focus?.overlay_key === "function" ? focus.overlay_key() : null;
            if (typeof overlay !== "string" || !overlay.length || overlay.length > 255 ||
                /[^\x20-\x7e]/.test(overlay))
                throw new Error("Focused window keyboard is unavailable");
            return {...inactive(""), keyboardOverlay : overlay};
        }
        const active = globalThis.FocusNavController?.m_ActiveContext?.m_activeWindow;
        const window =
            active?.SteamClient?.OpenVR?.Keyboard?.Show
                ? active
                : globalThis.g_PopupManager?.GetPopupForVROverlayKey("valve.steam.gamepadui.main")
                      ?.window;
        if (typeof window?.SteamClient?.OpenVR?.Keyboard?.Show !== "function")
            throw new Error("Steam VR keyboard is unavailable");
        // The native request needs an owning overlay, SharedJSContext has none.
        window.SteamClient.OpenVR.Keyboard.Show();
    }
    // A shortcut has no navigation lease and must leave the typing gate's owner alone.
    if (request.actionOnly)
        return inactive("");
    const keyboardComponent = () => {
        const window =
            globalThis.g_PopupManager?.GetPopupForVROverlayKey("valve.steam.gamepadui.keyboard")
                ?.window;
        for (const element of [...(window?.document?.querySelectorAll('[role="grid"]') || [])]
                 .slice(0, 8)) {
            let fiber = element[Object.keys(element).find(key => key.startsWith("__reactFiber$"))];
            for (let depth = 0; fiber && depth < 60; fiber = fiber.return, ++depth)
                if (fiber.stateNode?.m_trackpadInput)
                    return {component : fiber.stateNode, window};
        }
    };
    const watchKey = "__controllerutilsKeyboardWatchV1";
    const syncTyping = () => {
        let watch = globalThis[watchKey];
        if (watch && (watch.owner !== request.owner || watch.serial !== request.serial))
            throw new Error("Another controller owns keyboard input recovery");
        if (watch && watch.deadline <= now()) {
            watch.close();
            watch = undefined;
        }
        if (!request.enabled && !request.typing) {
            watch?.close();
            return false;
        }
        if (!watch && !request.typing)
            return false;
        const controllers = globalThis.ControllerStore?.GetControllers();
        const matches = controllers?.filter(c => c.strSerialNumber === request.serial &&
                                                 c.eControllerType === 10 &&
                                                 c.bBluetooth === true && !c.bRemoteDevice);
        if (matches?.length !== 1) {
            watch?.close();
            return false;
        }
        const index = matches[0].nControllerIndex;
        if (!Number.isInteger(index) || index < 0 || index >= 16 ||
            controllers.filter(c => c.nControllerIndex === index).length !== 1) {
            watch?.close();
            return false;
        }
        const keyboard = keyboardComponent();
        if (!keyboard || keyboard.window.document.hidden) {
            watch?.close();
            return false;
        }
        const {component, window} = keyboard, input = component.m_trackpadInput;
        if (watch && (watch.input !== input || watch.index !== index)) {
            watch.close();
            watch = undefined;
        }
        if (!watch) {
            const registration = input.RegisterForAnalog((pad, controller) => {
                if (controller === index && (pad === 19 || pad === 21))
                    watch.samples.set(pad, now());
            });
            let timer;
            watch = {
                owner : request.owner,
                serial : request.serial,
                input,
                index,
                deadline : 0,
                samples : new Map(),
                missingSince : undefined,
                repairs : 0,
                lastRepair : -Infinity,
                pending : false,
                state : "starting",
                renew() {
                    this.deadline = now() + 1000;
                    clearTimeout(timer);
                    timer = setTimeout(() => this.close(), 1000);
                },
                close() {
                    clearTimeout(timer);
                    registration.Unregister();
                    if (globalThis[watchKey] === this)
                        delete globalThis[watchKey];
                },
                rearm() {
                    input.EnableAnalogInputMessages(true);
                    window.SteamClient.Input.EnableControllerAnalogInputMessages(true);
                    globalThis.SteamClient.Input.SetKeyboardActionset(
                        true, component.props.windowInstance.IsStandaloneKeyboardWindow() ||
                                  component.props.windowInstance.IsDesktopOverlayWindow());
                    this.lastRepair = now();
                    ++this.repairs;
                }
            };
            globalThis[watchKey] = watch;
            watch.renew();
            watch.rearm();
        }
        watch.renew();
        const pads = [ 19, 21 ].filter((pad, i) => request.touches & 1 << i);
        const missing =
            request.typing && pads.some(pad => now() - (watch.samples.get(pad) ?? -Infinity) > 350);
        if (missing) {
            if (watch.missingSince === undefined)
                watch.missingSince = now();
            if (now() - watch.missingSince >= 350 && now() - watch.lastRepair >= 500) {
                if (watch.repairs < 2)
                    watch.rearm();
                else
                    watch.pending = true;
            }
            watch.state = watch.repairs > 1 ? "recovering" : "starting";
        } else if (request.typing && pads.length) {
            watch.missingSince = undefined;
            watch.pending = false;
            watch.state = "ready";
        }
        if (!request.typing) {
            // Reopen only after every typing control is released, preserving the same text target.
            if (watch.pending && request.touches === 0 && request.held === false) {
                const manager = component.props.VirtualKeyboardManager;
                const target = manager?.m_currentVirtualKeyboardRef,
                      props = manager?.m_ActiveElementProps;
                const modal = manager?.m_bIsVirtualKeyboardModal?.Value;
                if (target && props && typeof modal === "boolean" &&
                    typeof target.BIsElementValidForInput === "function" &&
                    target.BIsElementValidForInput() &&
                    typeof manager.SetVirtualKeyboardHidden === "function" &&
                    typeof manager.ShowVirtualKeyboard === "function") {
                    manager.SetVirtualKeyboardHidden();
                    setTimeout(() => {
                        if (!manager.m_currentVirtualKeyboardRef &&
                            manager.m_lastActiveVirtualKeyboardRef === target &&
                            target.BIsElementValidForInput())
                            manager.ShowVirtualKeyboard(target, props, modal);
                    }, 80);
                    console.info(
                        "Controller Utils: reopened keyboard after missing native pad input");
                }
            }
            watch.close();
            return false;
        }
        return true;
    };
    if (!request.enabled) {
        if (lease)
            lease.close();
        const keyboardWatch = syncTyping();
        return {
            ...inactive(""),
            keyboardWatch,
            keyboardHealth : globalThis[watchKey]?.state || "off"
        };
    }
    syncTyping();
    const nav = globalThis.FocusNavController;
    const store = globalThis.ControllerStore;
    if (!nav || !store || typeof store.GetControllers !== "function")
        throw new Error("Steam controller input context is unavailable");
    const identity = () => {
        try {
            return controllerIndex();
        } catch (_) {
            return -1;
        }
    };
    const index = identity();
    const keyboardSource = () => keyboardComponent()?.component.m_trackpadInput;
    if (lease && (lease.nav !== nav || lease.index !== index || nav[name] !== lease.wrapper ||
                  !nav.m_rgGamepadInputSources?.includes(lease.source))) {
        lease.close();
        return inactive("Controller identity or Steam input dispatch changed");
    }
    if (index < 0)
        return inactive("Bluetooth Steam Controller is disconnected or ambiguous");
    if (!lease) {
        if (Object.hasOwn(nav, name) || typeof nav[name] !== "function")
            throw new Error("Unexpected Steam input dispatch owner");
        // Compatibility fingerprint of the inspected dispatcher.
        // The privileged native gate separately pins its complete executable hash.
        const original = nav[name];
        if (!reviewed(original, 808, 0x1f133423))
            throw new Error("Unsupported Steam input dispatcher version");
        const sources = nav.m_rgGamepadInputSources?.filter(
            s => typeof s.HandleControllerInputMessages === "function" &&
                 s.m_rgControllers instanceof Map);
        if (!sources || sources.length !== 1)
            throw new Error("Unexpected Steam physical input source");
        const source = sources[0], active = source.m_rgControllers.get(index)?.activeButtons;
        // Do not swallow releases for button-down events delivered before acquisition.
        if (!active || Object.values(active).some(Boolean))
            return inactive("Release buttons and pads to enable controller menu input");
        let timer;
        lease = {
            owner : request.owner,
            serial : request.serial,
            index,
            nav,
            source,
            deadline : 0,
            keyboardGate : undefined,
            syncKeyboard() {
                const input = keyboardSource();
                if (this.keyboardGate?.input === input)
                    return;
                this.keyboardGate?.close(false);
                this.keyboardGate = undefined;
                if (!input)
                    return;
                const down = input.m_ButtonDownCallbacks, up = input.m_ButtonUpCallbacks;
                const analog = input.m_AnalogCallbacks;
                const dispatchers = [ down, up, analog ];
                if (dispatchers.some(callbacks => typeof callbacks?.Dispatch !== "function" ||
                                                  Object.hasOwn(callbacks, "Dispatch")))
                    throw new Error("Unexpected Steam keyboard input listener");
                const originals = dispatchers.map(callbacks => callbacks.Dispatch);
                const positions = new Map();
                const wrappers = dispatchers.map((callbacks, kind) => function(...args) {
                    if (now() >= lease.deadline || identity() !== index) {
                        lease.close();
                        return originals[kind].apply(this, args);
                    }
                    const selected =
                        kind === 2 ? args[1] === index : args[1] === 1 && args[2] === index;
                    if (selected && kind === 2)
                        positions.set(args[0], args);
                    // Native typing has a separate listener from navigation. Keep releases
                    // balanced.
                    if (selected && kind !== 1)
                        return;
                    return originals[kind].apply(this, args);
                });
                dispatchers.forEach(
                    (callbacks, kind) => Object.defineProperty(
                        callbacks, "Dispatch",
                        {value : wrappers[kind], writable : true, configurable : true}));
                this.keyboardGate = {
                    input,
                    close(replay) {
                        dispatchers.forEach((callbacks, kind) => {
                            if (callbacks.Dispatch === wrappers[kind])
                                delete callbacks.Dispatch;
                        });
                        // Other Steam windows can disable this shared stream while the keyboard
                        // stays mounted.
                        if (replay)
                            input.EnableAnalogInputMessages(true);
                        const controller = input.m_rgControllers.get(index);
                        if (replay && identity() === index)
                            for (const pad of [19, 21]) {
                                if (!controller?.activeButtons[pad])
                                    continue;
                                originals[0].call(down, pad, 1, index, false,
                                                  controller.controllerType,
                                                  controller.controllerStyle);
                                if (positions.has(pad))
                                    originals[2].apply(analog, positions.get(pad));
                            }
                    }
                };
                for (const pad of [19, 21])
                    originals[1].call(up, pad, 1, index);
            },
            close() {
                clearTimeout(timer);
                if (nav[name] === this.wrapper)
                    delete nav[name];
                if (globalThis[key] === this)
                    delete globalThis[key];
                this.keyboardGate?.close(true);
                this.keyboardGate = undefined;
            },
            wrapper(...args) {
                if (now() >= lease.deadline || identity() !== index ||
                    !nav.m_rgGamepadInputSources.includes(source)) {
                    lease.close();
                    return original.apply(this, args);
                }
                // Source 1 is physical Steam Input. Preserve the Steam/system keys.
                if (args[2] === 1 && args[3] === index && args[1] !== 27 && args[1] !== 28)
                    return;
                return original.apply(this, args);
            },
            renew() {
                this.deadline = now() + 1000;
                clearTimeout(timer);
                timer = setTimeout(() => this.close(), 1000);
            }
        };
        Object.defineProperty(nav, name,
                              {value : lease.wrapper, writable : true, configurable : true});
        globalThis[key] = lease;
    }
    lease.renew();
    lease.syncKeyboard();
    return {active : true, index, serial : request.serial, reason : ""};
})
