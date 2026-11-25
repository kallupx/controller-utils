CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -isystem third_party/openvr -Isrc -Ibuild
ARCH := $(if $(filter aarch64,$(shell uname -m)),linuxarm64,linux64)
RUFF ?= ruff
CLANG_FORMAT ?= clang-format
FORMAT_SOURCES := $(wildcard driver/*.cpp driver/*.h tools/*.cpp src/*/*.js tests/*.cpp tests/*.h tests/*.js src/*/*.cpp src/*/*.h src/*/*/*.cpp src/*/*/*.h)
SOURCES := $(filter-out src/app/main.cpp src/app/launch_main.cpp src/ui/renderer.cpp,$(wildcard src/app/*.cpp src/ui/*.cpp src/controller/*.cpp src/tracking/*.cpp src/interaction/*.cpp src/platform/*.cpp src/platform/linux/*.cpp src/steamvr/*.cpp))
OBJECTS := $(patsubst %.cpp,build/native/%.o,$(SOURCES))
SANITIZE_OBJECTS := $(patsubst %.cpp,build/sanitize/%.o,$(SOURCES))
LIBS := $(shell pkg-config --libs json-c libcrypto libcurl libsystemd) -ldl -pthread
UI_LIBS := $(shell pkg-config --libs egl gl cairo fontconfig)
SANITIZE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
UNIT_TESTS := calibration_test native_core_test controller_device_test controller_reader_test platform_runtime_test native_shortcuts_test child_process_test optical_session_test application_test application_instance_test registration_test native_host_test ui_presentation_test steam_ui_test resilience_gates_test
OTHER_TESTS := pose_math_test pointer_math_test body_pose_test mesh_test tracked_input_test gaze_test button_glyphs_test dashboard_test ui_renderer_test touchpad_glow_test

.PHONY: all check check-sanitize driver native-driver format check-format frame-optical
all: assets/controller.buttons native-driver build/controller-utils build/frame-launcher build/optical/frame_camera_hook.so build/optical/frame_input_hook.so

build/controller-utils: src/app/main.cpp $(OBJECTS) build/native/src/ui/renderer.o
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(LIBS) $(UI_LIBS)

build/frame-launcher: src/app/launch_main.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -o $@

check: build/controller-utils build/native_replay $(addprefix build/,$(UNIT_TESTS) $(OTHER_TESTS))
	python3 -m unittest discover -s tests -v
	node tests/steam_input_filter_test.js
	@set -e; for test in $(UNIT_TESTS) $(OTHER_TESTS); do ./build/$$test; done

check-sanitize: build/sanitize/native_replay $(addprefix build/sanitize/,$(UNIT_TESTS)) build/sanitize/ui_renderer_test build/sanitize/touchpad_glow_test
	@set -e; for test in $(UNIT_TESTS); do ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/sanitize/$$test; done
	__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/sanitize/ui_renderer_test
	__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/sanitize/touchpad_glow_test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 CONTROLLER_UTILS_REPLAY=$(CURDIR)/build/sanitize/native_replay python3 -m unittest discover -s tests -p test_behavior.py -v
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 CONTROLLER_UTILS_REPLAY=$(CURDIR)/build/sanitize/native_replay python3 -m unittest discover -s tests -p test_steam_ui_transport.py -v

format:
	$(RUFF) check --fix tools tests packaging
	$(RUFF) format tools tests packaging
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

check-format:
	$(RUFF) check tools tests packaging
	$(RUFF) format --check tools tests packaging
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

build/native/%.o: %.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -pthread $(shell pkg-config --cflags json-c libcrypto libcurl libsystemd egl gl cairo fontconfig) -MMD -MP -c $< -o $@

build/sanitize/%.o: %.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) -pthread $(shell pkg-config --cflags json-c libcrypto libcurl libsystemd egl gl cairo fontconfig) -MMD -MP -c $< -o $@

build/native/src/steamvr/steam_ui_transport.o build/sanitize/src/steamvr/steam_ui_transport.o: build/input_filter_js.h

-include $(OBJECTS:.o=.d) $(SANITIZE_OBJECTS:.o=.d) build/native/src/ui/renderer.d build/sanitize/src/ui/renderer.d

build/fake_openvr.so: tests/fake_openvr_library.cpp tests/fake_openvr_interfaces.h tests/fake_openvr_overlay.h third_party/openvr/openvr.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Wno-unused-parameter -fPIC -shared $< -o $@

build/optical_session_test build/sanitize/optical_session_test: WRAP := -Wl,--wrap=connect,--wrap=clock_gettime
build/native_host_test build/sanitize/native_host_test build/platform_runtime_test build/sanitize/platform_runtime_test: | build/fake_openvr.so
build/native_host_test build/sanitize/native_host_test build/controller_device_test build/sanitize/controller_device_test build/controller_reader_test build/sanitize/controller_reader_test: tests/fake_controller.h
build/native_replay build/sanitize/native_replay build/application_test build/sanitize/application_test: tests/app_replay.h
build/native_replay build/sanitize/native_replay build/application_test build/sanitize/application_test build/optical_session_test build/sanitize/optical_session_test: tests/session_replay.h tests/filter_fixture.h tests/fake_controller.h
build/registration_test build/sanitize/registration_test: tests/fake_openvr_interfaces.h

$(addprefix build/,$(UNIT_TESTS) native_replay): build/%: tests/%.cpp $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LIBS) $(WRAP)

$(addprefix build/sanitize/,$(UNIT_TESTS) native_replay): build/sanitize/%: tests/%.cpp $(SANITIZE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LIBS) $(WRAP)

build/ui_renderer_test: tests/ui_renderer_test.cpp $(OBJECTS) build/native/src/ui/renderer.o | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(LIBS) $(UI_LIBS)

build/sanitize/ui_renderer_test: tests/ui_renderer_test.cpp $(SANITIZE_OBJECTS) build/sanitize/src/ui/renderer.o | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(LIBS) $(UI_LIBS)

build/dashboard_test: tests/dashboard_test.cpp tests/fake_openvr_interfaces.h src/steamvr/controller_input.h src/ui/messages.cpp src/ui/messages.h src/platform/json.cpp src/ui/dashboard.h src/ui/alignment.h src/ui/pointer.h src/ui/button_glyphs.h src/ui/vr.h src/ui/body_pose.h src/interaction/pointer_math.h src/interaction/pose_math.h src/ui/gaze.h third_party/openvr/openvr.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags gl json-c cairo fontconfig) $(filter %.cpp,$^) -o $@ $(shell pkg-config --libs gl json-c cairo fontconfig) -ldl

build/button_glyphs_test: tests/button_glyphs_test.cpp src/ui/button_glyphs.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags cairo) $< -o $@ $(shell pkg-config --libs cairo)

build/gaze_test: tests/gaze_test.cpp src/ui/gaze.h src/interaction/pose_math.h third_party/openvr/openvr.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

assets/controller.buttons: tools/prepare_buttons.py assets/controller.stl assets/controller.surface.json
	python3 tools/prepare_buttons.py

build/body_pose_test: tests/body_pose_test.cpp src/platform/json.cpp src/ui/body_pose.h src/interaction/pose_math.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) $(shell pkg-config --cflags --libs json-c) -o $@

build/mesh_test: tests/mesh_test.cpp src/ui/mesh.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/touchpad_glow_test: tests/touchpad_glow_test.cpp src/ui/model_renderer.h src/ui/touchpad_glow.h src/ui/button_animation.h src/ui/mesh.h src/interaction/pointer_math.h src/interaction/pose_math.h assets/controller.buttons
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags egl gl cairo) $< -o $@ $(shell pkg-config --libs egl gl cairo)

build/sanitize/touchpad_glow_test: tests/touchpad_glow_test.cpp src/ui/model_renderer.h src/ui/touchpad_glow.h src/ui/button_animation.h src/ui/mesh.h src/interaction/pointer_math.h src/interaction/pose_math.h assets/controller.buttons
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags egl gl cairo) $< -o $@ $(shell pkg-config --libs egl gl cairo)

build/pose_math_test: tests/pose_math_test.cpp src/interaction/pose_math.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/pointer_math_test: tests/pointer_math_test.cpp src/interaction/pointer_math.h src/interaction/pose_math.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

driver: native-driver

build/tracked_input_test: tests/tracked_input_test.cpp tests/tracked_input_support.h driver/tracked_input.h driver/tip_cursor.h src/interaction/pointer_math.h src/interaction/pose_math.h third_party/openvr/openvr_driver.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/input_filter_js.h: src/steamvr/input_filter.js
	@mkdir -p build
	python3 -c 'import json,pathlib; pathlib.Path("$@").write_text("static constexpr char input_filter_source[] = " + json.dumps(pathlib.Path("$<").read_text()) + ";\n")'

native-driver: build/native-driver/bin/$(ARCH)/driver_controllerutils_native.so build/native-driver/driver.vrdrivermanifest build/native-driver/resources/input/sc26_profile.json build/native-driver/resources/input/vrcompositor_bindings_sc26.json build/native-driver/resources/rendermodels/sc26_input/sc26_input.obj

build/native-driver/bin/$(ARCH)/driver_controllerutils_native.so: driver/optical_driver.cpp driver/publish_input.h driver/tracked_input.h driver/tip_cursor.h src/interaction/pointer_math.h src/interaction/pose_math.h src/steamvr/optical_socket.h src/steamvr/protocol.h src/platform/linux/resource.h third_party/openvr/openvr_driver.h Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -shared -fvisibility=hidden $< -o $@

build/native-driver/driver.vrdrivermanifest: driver/native-driver.vrdrivermanifest
	mkdir -p $(@D)
	cp $< $@

build/native-driver/resources/input/%.json: driver/resources/input/%.json
	mkdir -p $(@D)
	cp $< $@

build/native-driver/resources/rendermodels/sc26_input/sc26_input.obj: driver/resources/rendermodels/sc26_input/sc26_input.obj
	mkdir -p $(@D)
	cp $< $@

build/optical/frame_palm_inference: tools/frame_palm_inference.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< -ldl -o $@

build/optical/frame_camera_hook.so: tools/frame_camera_hook.cpp src/platform/linux/resource.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) -shared -fPIC -fvisibility=hidden $< -o $@ $(shell pkg-config --libs json-c) -ldl -pthread

FRAME_INPUT_HOOK_SOURCES := tools/frame_input_hook.cpp src/steamvr/ui_gate.cpp src/platform/linux/files.cpp src/platform/linux/process.cpp src/platform/linux/resource.cpp
build/optical/frame_input_hook.so: $(FRAME_INPUT_HOOK_SOURCES) src/steamvr/optical_socket.h src/steamvr/ui_gate.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -fPIC -fvisibility=hidden $(FRAME_INPUT_HOOK_SOURCES) -Wl,--no-undefined -o $@ -lcrypto -pthread

build/optical/%.py: tools/%.py
	install -Dm644 $< $@

frame-optical: all build/frame-compatibility build/optical/frame_palm_inference $(addprefix build/optical/,frame_raw_camera.py frame_camera_runtime.py frame_camera_worker.py frame_led_tracking.py frame_palm_bootstrap.py frame_camera_calibration.py frame_debug.py)

build/frame-compatibility: tools/frame_compatibility.cpp $(filter-out tools/frame_input_hook.cpp,$(FRAME_INPUT_HOOK_SOURCES)) src/steamvr/ui_gate.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -lcrypto -o $@

build/frame_update_source.h: packaging/bootstrap.py
	@mkdir -p build
	python3 -c 'import json,pathlib; pathlib.Path("$@").write_text("static constexpr char frame_update_source[] = " + json.dumps(pathlib.Path("$<").read_text(), ensure_ascii=False) + ";\n")'

build/frame_migration_policies.h: packaging/70-controller-utils.rules packaging/controller-utils-frame.conf
	@mkdir -p build
	python3 -c 'import json,pathlib; pathlib.Path("$@").write_text("\n".join("static constexpr char " + name + "[] = " + json.dumps(pathlib.Path(path).read_text()) + ";" for name,path in zip(("frame_udev_policy","frame_keep_policy"),"$^".split()))+"\n")'

build/frame-legacy-launcher: tools/frame_legacy_launcher.cpp build/frame_update_source.h
	$(CXX) $(CXXFLAGS) $< -o $@

build/frame-legacy-cleanup: tools/frame_legacy_cleanup.cpp build/frame_migration_policies.h src/platform/linux/protected_directory.cpp src/platform/linux/files.cpp src/platform/linux/resource.cpp
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -lcrypto -o $@
