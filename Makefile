CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -isystem third_party/openvr -Isrc -Ibuild
ARCH := $(if $(filter aarch64,$(shell uname -m)),linuxarm64,linux64)
RUFF ?= ruff
CLANG_FORMAT ?= clang-format
FORMAT_SOURCES := $(wildcard driver/*.cpp driver/*.h tools/*.cpp src/*/*.js tests/*.cpp tests/*.h tests/*.js)
NATIVE_SOURCES := $(filter-out src/app/main.cpp src/helper/main.cpp src/update/launcher_main.cpp src/update/installer_main.cpp src/update/installer.cpp src/update/setup.cpp src/ui/renderer.cpp,$(wildcard src/app/*.cpp src/ui/*.cpp src/controller/*.cpp src/tracking/*.cpp src/interaction/*.cpp src/platform/*.cpp src/platform/linux/*.cpp src/steamvr/*.cpp src/helper/*.cpp src/update/*.cpp))
NATIVE_LIBS := $(shell pkg-config --libs json-c libcrypto libunwind-ptrace libcurl libarchive libsystemd) -ldl -pthread
HELPER_LIBS := $(shell pkg-config --libs json-c libcrypto libunwind-ptrace) -ldl -pthread
DRIVER_PROFILE_SOURCES := driver/compatibility_profile.cpp src/update/profile.cpp src/update/signature.cpp src/platform/json.cpp src/platform/linux/files.cpp src/platform/linux/resource.cpp src/platform/linux/protected_directory.cpp src/platform/linux/records.cpp src/platform/linux/process.cpp src/tracking/backend.cpp
NATIVE_OBJECTS := $(patsubst %.cpp,build/native/%.o,$(NATIVE_SOURCES))
HELPER_SOURCES := $(filter src/helper/%,$(NATIVE_SOURCES)) src/platform/json.cpp src/platform/linux/resource.cpp src/platform/linux/process.cpp src/platform/linux/files.cpp src/platform/linux/protected_directory.cpp src/platform/linux/signals.cpp src/tracking/acquisition.cpp
HELPER_OBJECTS := $(patsubst %.cpp,build/native/%.o,$(HELPER_SOURCES))
APP_SOURCES := $(filter-out src/helper/% src/platform/linux/process.cpp src/update/launcher.cpp,$(NATIVE_SOURCES))
APP_OBJECTS := $(patsubst %.cpp,build/native/%.o,$(APP_SOURCES)) build/native/src/ui/renderer.o
APP_LIBS := $(shell pkg-config --libs json-c libcrypto libcurl libarchive egl gl cairo fontconfig libsystemd) -ldl -pthread
LAUNCHER_SOURCES := src/app/instance.cpp src/app/settings.cpp src/tracking/backend.cpp src/platform/json.cpp src/platform/linux/resource.cpp src/platform/linux/files.cpp src/platform/linux/protected_directory.cpp src/platform/linux/records.cpp src/update/metadata.cpp src/update/signature.cpp src/update/compatibility.cpp src/update/archive.cpp src/update/directories.cpp src/update/state.cpp src/update/transaction.cpp src/update/launcher.cpp
LAUNCHER_OBJECTS := $(patsubst %.cpp,build/native/%.o,$(LAUNCHER_SOURCES))
INSTALLER_OBJECTS := build/native/src/update/installer.o build/native/src/update/setup.o build/native/src/update/repair.o
LAUNCHER_LIBS := $(shell pkg-config --libs json-c libcrypto libcurl libarchive) -pthread
SANITIZE_OBJECTS := $(patsubst %.cpp,build/sanitize/%.o,$(NATIVE_SOURCES))
SANITIZE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
NATIVE_TESTS := calibration_test native_core_test controller_device_test controller_reader_test native_transport_test platform_runtime_test native_shortcuts_test child_process_test tracking_session_test application_test application_instance_test registration_test native_host_test ui_presentation_test steam_ui_test update_metadata_test update_compatibility_test update_archive_test update_transaction_test update_launcher_test pinned_process_test trace_test gate_patch_test adapter_patch_test recovery_guard_test tracker_control_test native_package_test capability_publisher_test native_bootstrap_test supervisor_test xr_backend_test discovery_test
NATIVE_TESTS += resilience_validation_test resilience_validation_tail_test resilience_validation_module_test resilience_gates_test resilience_maintenance_profiles resilience_maintenance_client resilience_maintenance_repair
NATIVE_TESTS += public_interfaces_test startup_recovery_test
NATIVE_TESTS += input_gates_test
NATIVE_TESTS += steam_os_channel_test
SANITIZE_TESTS := $(NATIVE_TESTS) native_tracker_test native_queue_startup_test update_installer_test update_setup_test
FORMAT_SOURCES += $(wildcard src/*/*.cpp src/*/*.h src/*/*/*.cpp src/*/*/*.h)

.PHONY: all check driver native-driver format check-format
.PHONY: check-sanitize
all: assets/controller.buttons native-driver build/controller-utils-updater build/controller-utils-helper build/controller-utils-launcher build/controller-utils

build/native/src/ui/renderer.o: src/ui/renderer.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags egl gl json-c cairo fontconfig) -MMD -MP -c $< -o $@

-include build/native/src/ui/renderer.d

build/controller-utils: src/app/main.cpp $(APP_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(APP_LIBS)

build/controller-utils-launcher: src/update/launcher_main.cpp $(LAUNCHER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(LAUNCHER_LIBS)

build/update_installer_test: tests/update_installer_test.cpp tests/update_fixture.h tests/arm_platform.h $(INSTALLER_OBJECTS) $(LAUNCHER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LAUNCHER_LIBS) -Wl,--wrap=renameat2,--wrap=unlinkat,--wrap=uname

build/update_setup_test: tests/update_setup_test.cpp tests/update_fixture.h tests/arm_platform.h $(INSTALLER_OBJECTS) $(LAUNCHER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LAUNCHER_LIBS) -Wl,--wrap=uname

build/sanitize/update_setup_test: tests/update_setup_test.cpp tests/update_fixture.h tests/arm_platform.h $(patsubst build/native/%,build/sanitize/%,$(INSTALLER_OBJECTS)) $(patsubst build/native/%,build/sanitize/%,$(LAUNCHER_OBJECTS))
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LAUNCHER_LIBS) -Wl,--wrap=uname

build/controller-utils-updater: src/update/installer_main.cpp $(INSTALLER_OBJECTS) $(LAUNCHER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LAUNCHER_LIBS)

build/release_check: tools/release_check.cpp build/native/src/update/profile.o $(LAUNCHER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(LAUNCHER_LIBS)

build/controller-utils-helper: src/helper/main.cpp $(HELPER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c libcrypto libunwind-ptrace) $^ -o $@ $(HELPER_LIBS)

check: build/controller-utils-updater build/update_setup_test check-arm-loader build/controller-utils build/native_replay build/release_check build/update_installer_test $(addprefix build/,$(NATIVE_TESTS)) build/native_bridge_test build/native_bridge_recovery_test build/native_queue_startup_test build/pose_math_test build/pointer_math_test build/body_pose_test build/mesh_test build/tracked_input_test build/native_tracker_test build/tracker_capability_test build/gaze_test build/button_glyphs_test build/dashboard_test build/ui_renderer_test build/touchpad_glow_test
	python3 -m unittest discover -s tests -v
	node tests/steam_input_filter_test.js
	./build/update_setup_test
	./build/calibration_test
	./build/native_core_test
	./build/controller_device_test
	./build/controller_reader_test
	./build/native_transport_test
	./build/platform_runtime_test
	./build/public_interfaces_test
	./build/startup_recovery_test
	./build/native_shortcuts_test
	./build/child_process_test
	./build/tracking_session_test
	./build/application_test
	./build/application_instance_test
	./build/registration_test
	./build/native_host_test
	./build/ui_presentation_test
	./build/steam_ui_test
	./build/update_installer_test
	./build/update_metadata_test
	./build/update_compatibility_test
	./build/update_archive_test
	./build/update_transaction_test
	./build/update_launcher_test
	./build/pinned_process_test
	./build/trace_test
	./build/gate_patch_test
	./build/resilience_validation_test
	./build/resilience_validation_tail_test
	./build/resilience_validation_module_test
	./build/resilience_gates_test
	./build/resilience_maintenance_profiles
	./build/resilience_maintenance_client
	./build/resilience_maintenance_repair
	./build/adapter_patch_test
	./build/recovery_guard_test
	./build/tracker_control_test
	./build/native_package_test
	./build/capability_publisher_test
	./build/native_bootstrap_test
	./build/supervisor_test
	./build/xr_backend_test
	./build/discovery_test
	./build/input_gates_test
	./build/steam_os_channel_test
	./build/native_bridge_test
	./build/native_bridge_recovery_test
	./build/native_queue_startup_test
	./build/pose_math_test
	./build/pointer_math_test
	./build/body_pose_test
	./build/mesh_test
	./build/tracked_input_test
	./build/native_tracker_test
	./build/tracker_capability_test
	./build/gaze_test
	./build/button_glyphs_test
	./build/dashboard_test
	./build/ui_renderer_test
	./build/touchpad_glow_test

format:
	$(RUFF) format tools tests packaging/bootstrap.py
	$(RUFF) check --fix tools tests packaging/bootstrap.py
	$(RUFF) format tools tests packaging/bootstrap.py
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

check-format:
	$(RUFF) check tools tests packaging/bootstrap.py
	$(RUFF) format --check tools tests packaging/bootstrap.py
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

build/native/%.o: %.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -pthread $(shell pkg-config --cflags json-c libcrypto libunwind-ptrace libcurl libarchive libsystemd) -MMD -MP -c $< -o $@

build/native/src/steamvr/steam_ui_transport.o build/sanitize/src/steamvr/steam_ui_transport.o: build/input_filter_js.h

build/native/src/update/client.o build/native/src/app/host.o build/sanitize/src/update/client.o build/sanitize/src/app/host.o: build/update_public_key.h

build/native_replay: tests/native_replay.cpp tests/filter_fixture.h tests/input_control_replay.h tests/session_replay.h tests/app_replay.h tests/download_replay.h tests/update_client_replay.h tests/acquisition_replay.h tests/fake_controller.h tests/fake_native_queue.h $(NATIVE_OBJECTS) | build/fake_dns.so
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/native_core_test: tests/native_core_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/controller_device_test: tests/controller_device_test.cpp tests/fake_controller.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/controller_reader_test: tests/controller_reader_test.cpp tests/fake_controller.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/native_transport_test: tests/native_transport_test.cpp tests/fake_native_queue.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/fake_dns.so: tests/fake_dns.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -shared $< -o $@ -ldl

build/fake_openvr.so: tests/fake_openvr_library.cpp tests/fake_openvr_interfaces.h tests/fake_openvr_overlay.h third_party/openvr/openvr.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Wno-unused-parameter -fPIC -shared $< -o $@

build/fake_module.so: tests/fake_module.cpp
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -shared $< -o $@

build/resilience_validation_test build/resilience_validation_tail_test build/resilience_validation_module_test: build/%: tests/%.cpp driver/native_queue_startup.h src/steamvr/queue_abi.h src/steamvr/code.h src/steamvr/module.h src/steamvr/queue_validation.h src/steamvr/image.h | build/fake_module.so
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -lcrypto -ldl -pthread -o $@

build/resilience_gates_test: tests/resilience_gates_test.cpp src/helper/patch.h src/steamvr/code.h src/steamvr/image.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/platform_runtime_test: tests/platform_runtime_test.cpp $(NATIVE_OBJECTS) | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/public_interfaces_test: tests/public_interfaces_test.cpp src/steamvr/queue_api.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/native_shortcuts_test: tests/native_shortcuts_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/child_process_test: tests/child_process_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/tracking_session_test: tests/tracking_session_test.cpp tests/filter_fixture.h tests/session_replay.h tests/fake_controller.h tests/fake_native_queue.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/application_test: tests/application_test.cpp tests/filter_fixture.h tests/app_replay.h tests/session_replay.h tests/fake_controller.h tests/fake_native_queue.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/application_instance_test: tests/application_instance_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/registration_test: tests/registration_test.cpp tests/fake_openvr_interfaces.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/sanitize/registration_test: tests/fake_openvr_interfaces.h

build/native_host_test: tests/native_host_test.cpp tests/fake_controller.h $(NATIVE_OBJECTS) | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/sanitize/native_host_test: tests/fake_controller.h

build/steam_ui_test: tests/steam_ui_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/ui_renderer_test: tests/ui_renderer_test.cpp $(APP_OBJECTS) | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(APP_LIBS)

build/ui_presentation_test: tests/ui_presentation_test.cpp src/ui/presentation.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/update_metadata_test: tests/update_metadata_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c libcurl libcrypto) $^ -o $@ $(NATIVE_LIBS)

build/update_compatibility_test: tests/update_compatibility_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/update_archive_test: tests/update_archive_test.cpp tests/update_fixture.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c libarchive) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/update_transaction_test: tests/update_transaction_test.cpp tests/update_fixture.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c libarchive) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS) -Wl,--wrap=write,--wrap=fsync,--wrap=renameat,--wrap=renameat2

build/update_launcher_test: tests/update_launcher_test.cpp tests/update_fixture.h $(NATIVE_OBJECTS) | build/controller-utils-launcher
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c libarchive) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/pinned_process_test build/sanitize/pinned_process_test: NATIVE_LIBS += -Wl,--wrap=fstat,--wrap=fstatat

build/pinned_process_test: tests/pinned_process_test.cpp tests/process_image.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/trace_test: tests/trace_test.cpp tests/process_image.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) -fno-omit-frame-pointer $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/gate_patch_test: tests/gate_patch_test.cpp tests/patch_memory.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/resilience_maintenance_profiles build/resilience_maintenance_client build/resilience_maintenance_repair: build/%: tests/%.cpp tests/update_fixture.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/adapter_patch_test: tests/adapter_patch_test.cpp tests/patch_memory.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/recovery_guard_test: tests/recovery_guard_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/tracker_control_test: tests/tracker_control_test.cpp tests/patch_memory.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/native_package_test: tests/native_package_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/capability_publisher_test: tests/capability_publisher_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/native_bootstrap_test: tests/native_bootstrap_test.cpp tests/patch_memory.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/supervisor_test: tests/supervisor_test.cpp $(NATIVE_OBJECTS) | build/controller-utils-helper
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/xr_backend_test: tests/xr_backend_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/discovery_test: tests/discovery_test.cpp tests/process_image.h $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/input_gates_test: tests/input_gates_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/steam_os_channel_test: tests/steam_os_channel_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

-include $(NATIVE_OBJECTS:.o=.d) $(INSTALLER_OBJECTS:.o=.d) $(patsubst build/native/%,build/sanitize/%,$(INSTALLER_OBJECTS:.o=.d))
-include $(SANITIZE_OBJECTS:.o=.d)

build/sanitize/%.o: %.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -pthread $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c libcrypto libunwind-ptrace libcurl libarchive libsystemd) -MMD -MP -c $< -o $@

build/sanitize/native_replay: | build/fake_dns.so
build/sanitize/native_replay: tests/filter_fixture.h tests/input_control_replay.h tests/session_replay.h tests/app_replay.h tests/download_replay.h tests/update_client_replay.h tests/acquisition_replay.h tests/fake_controller.h tests/fake_native_queue.h
build/sanitize/controller_device_test: tests/fake_controller.h
build/sanitize/controller_reader_test: tests/fake_controller.h
build/sanitize/native_transport_test: tests/fake_native_queue.h
build/sanitize/tracking_session_test: tests/filter_fixture.h tests/session_replay.h tests/fake_controller.h tests/fake_native_queue.h
build/sanitize/application_test: tests/filter_fixture.h tests/app_replay.h tests/session_replay.h tests/fake_controller.h tests/fake_native_queue.h
build/sanitize/pinned_process_test: tests/process_image.h
build/sanitize/trace_test: tests/process_image.h
build/sanitize/gate_patch_test: tests/patch_memory.h
build/sanitize/adapter_patch_test: tests/patch_memory.h
build/sanitize/tracker_control_test: tests/patch_memory.h
build/sanitize/native_bootstrap_test: tests/patch_memory.h
build/sanitize/supervisor_test: | build/controller-utils-helper
build/sanitize/discovery_test: tests/process_image.h
build/sanitize/update_archive_test: tests/update_fixture.h
build/sanitize/update_launcher_test: tests/update_fixture.h | build/controller-utils-launcher

build/sanitize/update_installer_test: tests/update_installer_test.cpp tests/update_fixture.h tests/arm_platform.h $(patsubst build/native/%,build/sanitize/%,$(INSTALLER_OBJECTS)) $(patsubst build/native/%,build/sanitize/%,$(LAUNCHER_OBJECTS))
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c) $(filter %.cpp %.o,$^) -o $@ $(LAUNCHER_LIBS) -Wl,--wrap=renameat2,--wrap=unlinkat,--wrap=uname

build/sanitize/update_transaction_test: tests/update_transaction_test.cpp tests/update_fixture.h $(SANITIZE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c libarchive) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS) -Wl,--wrap=write,--wrap=fsync,--wrap=renameat,--wrap=renameat2

build/sanitize/%: tests/%.cpp $(SANITIZE_OBJECTS) | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c libcrypto) $(filter %.cpp %.o,$^) -o $@ $(NATIVE_LIBS)

build/sanitize/src/ui/renderer.o: src/ui/renderer.cpp Makefile
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags egl gl json-c cairo fontconfig) -MMD -MP -c $< -o $@

-include build/sanitize/src/ui/renderer.d

build/sanitize/ui_renderer_test: tests/ui_renderer_test.cpp $(patsubst %.cpp,build/sanitize/%.o,$(APP_SOURCES)) build/sanitize/src/ui/renderer.o | build/fake_openvr.so
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(APP_LIBS)

build/sanitize/native_tracker_test: tests/native_tracker_test.cpp driver/native_tracker.cpp src/tracking/native_abi.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) -pthread $< -Wl,--wrap=pthread_create -o $@

build/sanitize/native_queue_startup_test: tests/native_queue_startup_test.cpp driver/native_queue_startup.h src/steamvr/queue_abi.h src/steamvr/code.h src/steamvr/module.h src/steamvr/queue_validation.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $< -lcrypto -ldl -pthread -o $@

build/sanitize/resilience_validation_test build/sanitize/resilience_validation_tail_test build/sanitize/resilience_validation_module_test: build/sanitize/%: tests/%.cpp driver/native_queue_startup.h src/steamvr/queue_abi.h src/steamvr/code.h src/steamvr/module.h src/steamvr/queue_validation.h src/steamvr/image.h | build/fake_module.so
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) $< -lcrypto -ldl -pthread -o $@

check-sanitize: build/sanitize/native_replay $(addprefix build/sanitize/,$(SANITIZE_TESTS)) build/sanitize/ui_renderer_test build/sanitize/touchpad_glow_test
	@set -e; for test in $(SANITIZE_TESTS); do ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/sanitize/$$test; done
	__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/sanitize/ui_renderer_test
	__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/sanitize/touchpad_glow_test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 CONTROLLER_UTILS_REPLAY=$(CURDIR)/build/sanitize/native_replay python3 -m unittest discover -s tests -p test_behavior.py -v
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 CONTROLLER_UTILS_REPLAY=$(CURDIR)/build/sanitize/native_replay python3 -m unittest discover -s tests -p test_native_download.py -v
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 CONTROLLER_UTILS_REPLAY=$(CURDIR)/build/sanitize/native_replay python3 -m unittest discover -s tests -p test_native_update_client.py -v
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 CONTROLLER_UTILS_REPLAY=$(CURDIR)/build/sanitize/native_replay python3 -m unittest discover -s tests -p test_steam_ui_transport.py -v

build/dashboard_test: tests/dashboard_test.cpp src/ui/messages.cpp src/ui/messages.h src/platform/json.cpp src/ui/dashboard.h src/ui/alignment.h src/ui/pointer.h src/ui/button_glyphs.h src/ui/vr.h src/ui/body_pose.h src/interaction/pointer_math.h src/interaction/pose_math.h src/ui/gaze.h third_party/openvr/openvr.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags gl json-c cairo fontconfig) $(filter %.cpp,$^) -o $@ $(shell pkg-config --libs gl json-c cairo fontconfig) -ldl

build/button_glyphs_test: tests/button_glyphs_test.cpp src/ui/button_glyphs.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags cairo) $< -o $@ $(shell pkg-config --libs cairo)

build/gaze_test: tests/gaze_test.cpp src/ui/gaze.h src/interaction/pose_math.h third_party/openvr/openvr.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/tracker_capability_test: tests/tracker_capability_test.cpp driver/tracker_capability.h src/tracking/native_abi.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/native_tracker_test: tests/native_tracker_test.cpp driver/native_tracker.cpp src/tracking/native_abi.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -pthread $< -Wl,--wrap=pthread_create -o $@

.PHONY: native-tracker
native-tracker:
	python3 tools/build_native_tracker.py $(if $(XR_SERVICE),--xrservice "$(XR_SERVICE)")

assets/controller.buttons: tools/prepare_buttons.py assets/controller.stl assets/controller.surface.json
	python3 tools/prepare_buttons.py

build/body_pose_test: tests/body_pose_test.cpp src/ui/body_pose.h src/interaction/pose_math.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -ljson-c -o $@

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

build/native_queue_startup_test: tests/native_queue_startup_test.cpp driver/native_queue_startup.h src/steamvr/queue_abi.h src/steamvr/code.h src/steamvr/module.h src/steamvr/queue_validation.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -lcrypto -ldl -pthread -o $@

build/native_bridge_test: tests/native_bridge_test.cpp tests/tracked_input_support.h driver/tracked_input.h driver/tip_cursor.h src/interaction/pointer_math.h src/interaction/pose_math.h driver/native_bridge.cpp src/steamvr/startup_recovery.h src/steamvr/protocol.h driver/tracker_capability.h src/tracking/native_abi.h driver/native_queue_startup.h src/steamvr/queue_abi.h third_party/openvr/openvr_driver.h src/steamvr/code.h src/steamvr/module.h src/steamvr/queue_validation.h $(DRIVER_PROFILE_SOURCES) src/update/profiles.h src/update/metadata.h build/update_public_key.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< $(DRIVER_PROFILE_SOURCES) -ljson-c -lcrypto -ldl -pthread -o $@

native-driver: build/native-driver/bin/$(ARCH)/driver_controllerutils_native.so build/native-driver/driver.vrdrivermanifest build/native-driver/resources/input/sc26_profile.json build/native-driver/resources/input/vrcompositor_bindings_sc26.json build/native-driver/resources/rendermodels/sc26_input/sc26_input.obj

build/native-driver/bin/$(ARCH)/driver_controllerutils_native.so: driver/native_bridge.cpp src/steamvr/startup_recovery.h driver/tracked_input.h driver/tip_cursor.h src/interaction/pointer_math.h src/interaction/pose_math.h src/steamvr/protocol.h driver/tracker_capability.h src/tracking/native_abi.h driver/native_queue_startup.h src/steamvr/queue_abi.h third_party/openvr/openvr_driver.h src/steamvr/code.h src/steamvr/module.h src/steamvr/queue_validation.h $(DRIVER_PROFILE_SOURCES) src/update/profiles.h src/update/metadata.h build/update_public_key.h
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -shared -fvisibility=hidden $< $(DRIVER_PROFILE_SOURCES) -ljson-c -lcrypto -ldl -pthread -o $@

build/native-driver/driver.vrdrivermanifest: driver/native-driver.vrdrivermanifest
	mkdir -p $(@D)
	cp $< $@

build/native-driver/resources/input/%.json: driver/resources/input/%.json
	mkdir -p $(@D)
	cp $< $@

build/native-driver/resources/rendermodels/sc26_input/sc26_input.obj: driver/resources/rendermodels/sc26_input/sc26_input.obj
	mkdir -p $(@D)
	cp $< $@

.PHONY: check-arm-loader
check-arm-loader: native-tracker
	python3 tests/check_native_tracker_arm.py

build/calibration_test: tests/calibration_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c) $^ -o $@ $(NATIVE_LIBS)

build/update_public_key.h: assets/update-public.pem
	mkdir -p $(@D)
	python3 -c 'import json,pathlib; p=pathlib.Path("assets/update-public.pem"); pathlib.Path("$@").write_text("#pragma once\ninline constexpr char UpdatePublicKey[] = " + json.dumps(p.read_text()) + ";\n")'

build/controller-utils-updater: build/update_public_key.h

build/startup_recovery_test: tests/startup_recovery_test.cpp $(NATIVE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags json-c libcrypto) $^ -o $@ $(NATIVE_LIBS)

build/native_bridge_recovery_test: tests/native_bridge_recovery_test.cpp driver/native_bridge.cpp src/steamvr/startup_recovery.h $(DRIVER_PROFILE_SOURCES) build/update_public_key.h
	$(CXX) $(CXXFLAGS) $< $(DRIVER_PROFILE_SOURCES) -ljson-c -lcrypto -ldl -pthread -o $@
