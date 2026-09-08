add_library(CemuWebRpc STATIC
  MainWindowState.cpp
  MainWindowState.h
  RpcDispatcher.cpp
  RpcDispatcher.h
	UpdatePlanRegistry.cpp
	UpdatePlanRegistry.h
)

add_library(CemuWebRendererAdapters STATIC
	OpenGLHost.h
  RendererHost.cpp
  RendererHost.h
)
if(ENABLE_OPENGL)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_sources(CemuWebRendererAdapters PRIVATE OpenGLHostLinux.cpp)
	elseif(WIN32)
		target_sources(CemuWebRendererAdapters PRIVATE OpenGLHostWin.cpp)
	elseif(APPLE)
		target_sources(CemuWebRendererAdapters PRIVATE OpenGLHostCocoa.mm)
		target_link_libraries(CemuWebRendererAdapters PRIVATE "-framework Cocoa" "-framework OpenGL")
	endif()
endif()
target_include_directories(CemuWebRendererAdapters PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(CemuWebRendererAdapters PRIVATE
  CemuCafe
  CemuCommon
  CemuConfig
  CemuHostContracts
)

option(CEMU_REGENERATE_WEB_UI "Regenerate embedded React assets during the C++ build" OFF)
set(CEMU_WEB_ASSET_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/generated/WebAssets.h")
if(CEMU_REGENERATE_WEB_UI)
  find_program(BUN_EXECUTABLE bun REQUIRED)
  add_custom_target(cemu_web_assets
    COMMAND "${BUN_EXECUTABLE}" install --frozen-lockfile
    COMMAND "${BUN_EXECUTABLE}" run generate
    COMMAND "${BUN_EXECUTABLE}" run build:embedded
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/ui"
    BYPRODUCTS "${CEMU_WEB_ASSET_HEADER}"
    VERBATIM
  )
elseif(NOT EXISTS "${CEMU_WEB_ASSET_HEADER}")
  message(FATAL_ERROR "Embedded web assets are missing; run 'cd ui && bun run build:embedded'")
endif()
target_include_directories(CemuWebRpc PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  ${RAPIDJSON_INCLUDE_DIRS}
)

add_library(CemuWebFrontend STATIC
	NativeKeyboardInput.h
	NativeWindowHost.h
	NativeFileDialog.h
	RendererHost.h
	ToolWindowSupport.h
	WebFrontend.cpp
	WebHostServices.cpp
	WebHostServices.h
	WebHostState.cpp
	WebHostState.h
  generated/RpcMethods.h
  generated/WindowRoles.h
  generated/WebAssets.h
)
if(CEMU_OVERLAY_BACKEND STREQUAL "cef")
	target_sources(CemuWebFrontend PRIVATE
	CemodWebUiFrontend.cpp
	CemodWebUiFrontend.h
	CemodNetworkPolicy.cpp
	CemodNetworkPolicy.h
	CemodNetworkProxy.cpp
	CemodNetworkProxy.h
	cef/CefNativeUiLoop.cpp
	cef/CefNativeUiLoop.h
    cef/CefOverlayFrameMailbox.cpp
    cef/CefOverlayFrameMailbox.h
	cef/CefOverlayInput.h
    cef/CefOverlayRuntime.cpp
    cef/CefOverlayRuntime.h
  )
  target_link_libraries(CemuWebFrontend PRIVATE CemuCefRuntime cemuextend::wire OpenSSL::Crypto)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(CemuWebFrontend PRIVATE NativeWindowHostGtk.cpp NativeFileDialogGtk.cpp ToolWindowSupportGtk.cpp)
  target_include_directories(CemuWebFrontend PRIVATE ${GTK3_INCLUDE_DIRS})
  if(NOT X11_Xi_FOUND)
    message(FATAL_ERROR "Linux native raw mouse input requires XInput2 (libXi)")
  endif()
  target_link_libraries(CemuWebFrontend PRIVATE ${GTK3_LIBRARIES} ${X11_Xi_LIB})
elseif(WIN32)
  target_sources(CemuWebFrontend PRIVATE NativeWindowHostWin.cpp NativeFileDialogWin.cpp ToolWindowSupportWin.cpp)
  target_link_libraries(CemuWebFrontend PRIVATE comdlg32 imm32 shell32 ole32)
elseif(APPLE)
  enable_language(OBJCXX)
	target_sources(CemuWebFrontend PRIVATE NativeWindowHostCocoa.mm NativeFileDialogCocoa.mm ToolWindowSupportCocoa.mm)
	target_link_libraries(CemuWebFrontend PRIVATE "-framework Cocoa" "-framework CoreFoundation" "-framework CoreGraphics" "-framework QuartzCore")
endif()
if(TARGET cemu_web_assets)
  add_dependencies(CemuWebFrontend cemu_web_assets)
endif()
target_include_directories(CemuWebFrontend PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  ${RAPIDJSON_INCLUDE_DIRS}
)
target_link_libraries(CemuWebFrontend PRIVATE
  CemuApplication
  CemuCommon
  CemuFrontendModel
  CemuHostContracts
  CemuInput
	CemuRuntimeOverlay
  CemuWebRpc
  CemuUtil
  CemuWebRendererAdapters
  PNG::PNG
)

if(BUILD_TESTING)
	add_executable(input_focus_state_tests tests/InputFocusStateTests.cpp)
	target_include_directories(input_focus_state_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
	add_test(NAME input_focus_state_tests COMMAND input_focus_state_tests)
	add_executable(native_keyboard_input_tests tests/NativeKeyboardInputTests.cpp)
	target_include_directories(native_keyboard_input_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
	add_test(NAME native_keyboard_input_tests COMMAND native_keyboard_input_tests)

	if(UNIX AND NOT APPLE)
		add_executable(gtk_native_input_contract_tests tests/GtkNativeInputContractTests.cpp)
		target_compile_definitions(gtk_native_input_contract_tests PRIVATE
			CEMU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
		add_test(NAME gtk_native_input_contract_tests COMMAND gtk_native_input_contract_tests)
	endif()

	if(CEMU_OVERLAY_BACKEND STREQUAL "cef")
		add_executable(cef_overlay_frame_mailbox_tests
			cef/CefOverlayFrameMailbox.cpp
			cef/tests/CefOverlayFrameMailboxTests.cpp)
		target_include_directories(cef_overlay_frame_mailbox_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
		add_test(NAME cef_overlay_frame_mailbox_tests COMMAND cef_overlay_frame_mailbox_tests)

		add_executable(cef_overlay_osr_smoke_tests cef/tests/CefOverlayOsrSmokeTests.cpp)
		target_include_directories(cef_overlay_osr_smoke_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
		target_link_libraries(cef_overlay_osr_smoke_tests PRIVATE CemuWebFrontend)
		if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
			target_include_directories(cef_overlay_osr_smoke_tests PRIVATE ${GTK3_INCLUDE_DIRS})
			target_link_libraries(cef_overlay_osr_smoke_tests PRIVATE ${GTK3_LIBRARIES})
		endif()
		cemu_configure_cef_runtime_target(cef_overlay_osr_smoke_tests)
		if(CEMU_STAGE_CEF_RUNTIME)
			cemu_stage_cef_runtime(cef_overlay_osr_smoke_tests)
		endif()
		find_program(XVFB_RUN_EXECUTABLE xvfb-run)
		if(XVFB_RUN_EXECUTABLE)
			add_test(NAME cef_overlay_osr_smoke_tests COMMAND
				"${XVFB_RUN_EXECUTABLE}" -a -s "-screen 0 1280x720x24"
				$<TARGET_FILE:cef_overlay_osr_smoke_tests>)
		else()
			add_test(NAME cef_overlay_osr_smoke_tests COMMAND cef_overlay_osr_smoke_tests)
		endif()
		set_tests_properties(cef_overlay_osr_smoke_tests PROPERTIES
			ENVIRONMENT "CEMU_CEF_NO_SANDBOX=1;CEMU_CEF_OSR_SMOKE=1"
			LABELS "cef-runtime"
			TIMEOUT 30)

		add_executable(cemod_network_policy_tests
			CemodNetworkPolicy.cpp
			tests/CemodNetworkPolicyTests.cpp)
		target_include_directories(cemod_network_policy_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
		target_link_libraries(cemod_network_policy_tests PRIVATE Boost::headers)
		add_test(NAME cemod_network_policy_tests COMMAND cemod_network_policy_tests)

		add_executable(cemod_network_proxy_tests
			CemodNetworkPolicy.cpp
			CemodNetworkProxy.cpp
			tests/CemodNetworkProxyTests.cpp)
		target_include_directories(cemod_network_proxy_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
		target_link_libraries(cemod_network_proxy_tests PRIVATE Boost::headers Threads::Threads)
		add_test(NAME cemod_network_proxy_tests COMMAND cemod_network_proxy_tests)
	endif()
  add_executable(web_main_window_state_tests tests/MainWindowStateTests.cpp)
  target_link_libraries(web_main_window_state_tests PRIVATE CemuWebRpc)
  add_test(NAME web_main_window_state_tests COMMAND web_main_window_state_tests)

  add_executable(web_rpc_dispatcher_tests tests/RpcDispatcherTests.cpp)
  target_link_libraries(web_rpc_dispatcher_tests PRIVATE CemuWebRpc)
  add_test(NAME web_rpc_dispatcher_tests COMMAND web_rpc_dispatcher_tests)

	add_executable(web_host_state_tests WebHostState.cpp tests/WebHostStateTests.cpp)
	target_include_directories(web_host_state_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
	target_link_libraries(web_host_state_tests PRIVATE CemuHostContracts)
	add_test(NAME web_host_state_tests COMMAND web_host_state_tests)

	add_executable(web_update_plan_registry_tests tests/UpdatePlanRegistryTests.cpp)
	target_link_libraries(web_update_plan_registry_tests PRIVATE CemuWebRpc)
	add_test(NAME web_update_plan_registry_tests COMMAND web_update_plan_registry_tests)
endif()
