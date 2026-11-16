#include "tracked_input_support.h"

struct Fixture {
    Host host;
    Input input;
    Properties properties;
    ct::TrackedInput device{&host, &input, &properties};
    vr::DriverPose_t pose{};
    ct::TrackedInputState state;
    Fixture() {
        pose.qRotation.w = pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = 1;
        pose.deviceIsConnected = pose.poseIsValid = true;
        pose.result = vr::TrackingResult_Running_OK;
        state.enabled = true;
        state.time = 10;
        assert(device.register_device("TEST1234"));
    }
    void tick(double time) {
        state.time = time;
        device.update(pose, time - .01, state, time);
    }
    void check_released() {
        assert(!device.active() && !host.published.deviceIsConnected &&
               !host.published.poseIsValid);
        assert(input.released() && properties.role() == vr::TrackedControllerRole_OptOut);
    }
};

int main() {
    Fixture f;
    f.check_released();
    assert(f.host.serial == "controllerutils-sc26-input-TEST1234");
    assert(f.device.register_device("TEST1234") && !f.device.register_device("OTHER123"));
    assert(f.host.adds == 1);
    assert(vr::CVRPropertyHelpers(&f.properties)
               .GetInt32Property(110, vr::Prop_ControllerHandSelectionPriority_Int32) == -1000);
    vr::ETrackedPropertyError model_error;
    assert(vr::CVRPropertyHelpers(&f.properties)
               .GetStringProperty(110, vr::Prop_RenderModelName_String, &model_error) ==
           "{controllerutils_native}sc26_input");
    assert(model_error == vr::TrackedProp_Success);
    assert(std::abs(f.input.tip.m[1][3] + .001f) < 1e-7 &&
           std::abs(f.input.tip.m[2][3] + .006f) < 1e-7);
    f.tick(10);
    assert(f.device.active() && f.properties.role() == vr::TrackedControllerRole_RightHand);
    assert(f.input.values.at("/input/activate/click") == 1);
    assert(f.input.values.at("/input/trigger/click") == 0);
    assert(std::abs(f.host.published.poseTimeOffset) < 1e-9);
    f.state.trigger = .6f;
    f.state.grip = .8f;
    f.state.stick_x = -.4f;
    f.state.back = true;
    f.tick(10.01);
    assert(f.input.values.at("/input/trigger/click") == 1 &&
           f.input.values.at("/input/b/click") == 1);
    auto updates = f.input.updates.size();
    f.device.update(f.pose, 10.01, f.state, 10.02);
    assert(f.input.updates.size() == updates);
    f.device.update(f.pose, 10.2, f.state, 10.2);
    f.check_released();
    f.tick(10.21);
    assert(f.device.active() && f.input.released());
    f.state.trigger = f.state.grip = f.state.stick_x = 0;
    f.state.back = false;
    f.tick(10.22);
    f.state.trigger = .6f;
    f.tick(10.23);
    assert(f.input.values.at("/input/trigger/click") == 1);

    f.properties.controller(2, vr::TrackedControllerRole_RightHand);
    f.host.poses[2].bDeviceIsConnected = true;
    f.tick(10.25);
    assert(f.device.active());
    f.state.enabled = false;
    f.tick(10.29);
    f.check_released();

    Fixture stale;
    stale.tick(20);
    stale.state.trigger = .8f;
    stale.tick(20.01);
    stale.state.time = 20.2;
    stale.device.update(stale.pose, 20, stale.state, 20.2);
    stale.check_released();
    stale.tick(20.21);
    stale.state.trigger = 0;
    stale.tick(20.22);
    stale.input.fail_once = true;
    stale.state.trigger = .8f;
    stale.tick(20.23);
    stale.check_released();
    stale.state.trigger = 0;
    stale.tick(20.24);
    stale.device.EnterStandby();
    stale.check_released();
    stale.tick(20.25);
    stale.device.Deactivate();
    assert(stale.device.device_index() == vr::k_unTrackedDeviceIndexInvalid &&
           stale.input.released());
    const int publishes = stale.host.publishes;
    stale.tick(20.26);
    assert(stale.host.publishes == publishes);

    Fixture selection;
    selection.tick(25);
    assert(selection.input.values.at("/input/activate/click") == 1);
    for (int i = 1; i <= 30; ++i) {
        selection.state.pad_touch = i % 2;
        selection.state.scroll_touch = !selection.state.pad_touch;
        selection.tick(25 + i * .01);
        assert(selection.input.values.at("/input/activate/click") == 1);
        assert(selection.input.values.at("/input/trigger/click") == 0);
        assert(selection.input.values.at("/input/cursorpad/click") == 0);
    }
    selection.state.enabled = false;
    selection.tick(25.31);
    selection.check_released();
    selection.state.enabled = true;
    selection.tick(25.32);
    assert(selection.input.values.at("/input/activate/click") == 1);

    Fixture cursor;
    cursor.state.pad_touch = cursor.state.scroll_touch = true;
    cursor.tick(30);
    assert(cursor.input.values.at("/input/activate/click") == 1);
    cursor.state.scroll_touch = false;
    cursor.tick(30.01);
    cursor.state.pad_x = .03f;
    cursor.tick(30.02);
    assert(cursor.input.tip.m[0][3] == 0);
    cursor.state.pad_x = .05f;
    cursor.tick(30.03);
    const auto world_tip = [&] {
        ct::Matrix local = ct::identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                local[c * 4 + r] = cursor.input.tip.m[r][c];
        ct::Matrix body{0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
        for (int i = 0; i < 3; ++i)
            body[12 + i] = cursor.pose.vecPosition[i];
        return ct::multiply(body, local);
    };
    const auto almost_same = [](const ct::Matrix &a, const ct::Matrix &b) {
        for (unsigned i = 0; i < a.size(); ++i)
            assert(std::abs(a[i] - b[i]) < 2e-6);
    };
    cursor.pose.qRotation = {std::sqrt(.5), 0, std::sqrt(.5), 0};
    cursor.pose.vecPosition[0] = .25;
    cursor.pose.vecPosition[1] = .1;
    cursor.pose.vecPosition[2] = -.2;
    cursor.tick(30.04);
    almost_same(world_tip(), ct::TipCursor::offset());
    assert(cursor.host.published.qRotation.y == cursor.pose.qRotation.y);
    assert(cursor.host.published.vecPosition[0] == .25);
    cursor.pose.vecPosition[0] += .1;
    cursor.device.update(cursor.pose, 30.041, cursor.state, 30.05);
    almost_same(world_tip(), ct::TipCursor::offset());
    for (int i = 0; i < 100; ++i) {
        cursor.state.pad_x = .05f + (i % 2 ? .002f : -.002f);
        cursor.state.pad_y = i % 2 ? .003f : -.003f;
        cursor.pose.vecPosition[0] += .001;
        cursor.tick(30.06 + i * .01);
        almost_same(world_tip(), ct::TipCursor::offset());
    }
    cursor.state.pad_x = .3f;
    cursor.state.pad_y = .2f;
    cursor.state.pad_click = true;
    cursor.tick(31.07);
    char diagnostic[128]{};
    cursor.device.DebugRequest("cursor_status", diagnostic, sizeof(diagnostic));
    assert(std::string(diagnostic).find("\"flags\":15") != std::string::npos);
    const auto swiped = world_tip();
    assert(-swiped[8] > .05f && -swiped[9] > .05f);
    assert(cursor.input.values.at("/input/cursorpad/click") == 1);
    cursor.state.pad_touch = false;
    cursor.pose.vecPosition[0] += .1;
    cursor.tick(31.075);
    almost_same(world_tip(), swiped);
    cursor.state.pad_touch = cursor.state.pad_click = false;
    cursor.tick(31.08);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            assert(std::abs(cursor.input.tip.m[r][c] - ct::TipCursor::offset()[c * 4 + r]) < 1e-7);
    assert(cursor.input.values.at("/input/cursorpad/touch") == 0);
    assert(cursor.input.values.at("/input/cursorpad/click") == 0);

    cursor.state.scroll_touch = true;
    cursor.state.scroll_x = .2f;
    cursor.state.scroll_y = .3f;
    cursor.tick(31.09);
    const auto x = cursor.input.values.at("/input/trackpad/x"),
               y = cursor.input.values.at("/input/trackpad/y");
    for (int i = 0; i < 50; ++i) {
        cursor.state.scroll_x = .2f + (i % 2 ? .002f : -.002f);
        cursor.state.scroll_y = .3f + (i % 2 ? .002f : -.002f);
        cursor.tick(31.1 + i * .01);
        assert(cursor.input.values.at("/input/trackpad/x") == x &&
               cursor.input.values.at("/input/trackpad/y") == y);
    }
    cursor.state.scroll_y = .7f;
    cursor.state.scroll_click = true;
    cursor.tick(31.6);
    assert(cursor.input.values.at("/input/trackpad/y") > y &&
           cursor.input.values.at("/input/trackpad/y") < .7);
    assert(cursor.input.values.at("/input/trackpad/click") == 1);
    cursor.state.enabled = false;
    cursor.tick(31.61);
    cursor.check_released();
    cursor.device.DebugRequest("cursor_status", diagnostic, sizeof(diagnostic));
    assert(std::string(diagnostic).find("\"flags\":0") != std::string::npos);
    cursor.state.enabled = true;
    cursor.tick(31.62);
    assert(cursor.device.active() && cursor.input.released());
    cursor.state.scroll_touch = cursor.state.scroll_click = false;
    cursor.tick(31.63);
    cursor.state.pad_click = true;
    cursor.tick(31.64);
    assert(cursor.input.values.at("/input/cursorpad/click") == 1);
    cursor.state.pad_x = std::nanf("");
    cursor.tick(31.65);
    cursor.check_released();

    Fixture click;
    click.tick(40);
    click.state.grip = .6f;
    click.tick(40.01);
    click.state.grip = .47f;
    click.tick(40.02);
    assert(click.input.values.at("/input/grip/click") == 1);
    click.state.grip = .44f;
    click.tick(40.03);
    assert(click.input.values.at("/input/grip/click") == 0);
    click.state.pad_click = true;
    click.tick(40.04);
    click.input.fail_pose_once = true;
    click.tick(40.05);
    click.check_released();
    click.tick(40.06);
    assert(click.device.active() && click.input.released());
    std::puts("tracked input: identity, native tip, role yielding, freshness, neutral rearm and "
              "cleanup passed");
}
