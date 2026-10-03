# state-designer app target. The repo-root CMakeLists.txt is the build entry
# point: it resolves the ordo framework and adds this directory.

# Directory scope: the app's sources need moc and rcc; ordo's targets manage
# their own.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Svg)

add_executable(state-designer
    main.cpp
    resources/icons.qrc
    resources/brand.qrc
    harness/probe_support.cpp
    harness/probe_scenarios_shell.cpp
    harness/probe_scenarios_canvas.cpp
    harness/probe_scenarios_hierarchy.cpp
    harness/probe_scenarios_polish.cpp
    harness/probe_stage.cpp
    harness/probe_scene_index_check.cpp
    harness/smoke_domain.cpp
    harness/smoke_expression.cpp
    harness/smoke_expression_integration.cpp
    harness/smoke_expression_actors.cpp
    harness/smoke_sim.cpp
    harness/smoke_sim_hierarchy.cpp
    harness/smoke_sim_replay.cpp
    harness/smoke_sim_features.cpp
    harness/smoke_codegen.cpp
    harness/smoke_codegen_core.cpp
    harness/smoke_codegen_features.cpp
    harness/smoke_codegen_projector.cpp
    harness/smoke_pill_port.cpp
    harness/smoke_auto_layout.cpp
    harness/smoke_validator.cpp
    harness/smoke_xstate_interop.cpp
    harness/smoke_interaction_fsm.cpp
    harness/smoke_shell.cpp
    harness/smoke_note_editor.cpp
    harness/smoke_settings.cpp
    harness/smoke_recent_projects.cpp
    harness/smoke_io.cpp
    harness/smoke_scxml_io.cpp
    harness/smoke_mcp.cpp
    harness/cli_modes.cpp
    model/machine_doc.cpp
    model/sim_agent.cpp
    model/sim_agent_topology.cpp
    model/sim_agent_exec.cpp
    model/undo_store.cpp
    controller/edit_commands.cpp
    controller/setting_commands.cpp
    controller/sim_commands.cpp
    controller/undo_capture.cpp
    controller/undo_commands.cpp
    infra/code_generator.cpp
    infra/code_generator_common.cpp
    infra/code_generator_aux.cpp
    infra/code_generator_flat.cpp
    infra/code_generator_hier.cpp
    infra/node_code_projector.cpp
    infra/settings_store.cpp
    infra/settings_registry.cpp
    infra/recent_projects.cpp
    infra/io_dispatcher.cpp
    infra/expression.cpp
    infra/expression_parser.cpp
    infra/expression_typecheck.cpp
    infra/expression_eval.cpp
    infra/expression_emit.cpp
    infra/logic_inventory.cpp
    infra/machine_validator.cpp
    infra/project_io.cpp
    infra/xstate_v5_io.cpp
    infra/machine_io.cpp
    infra/plantuml_io.cpp
    infra/scxml_io.cpp
    infra/sim_clock.cpp
    infra/crash_handler.cpp
    infra/mcp_runtime_server.cpp
    infra/mcp_runtime_server_batch.cpp
    view/shell/activity_rail.cpp
    view/geometry/edge_router.cpp
    view/geometry/routing_logger.cpp
    view/geometry/pill_port_resolver.cpp
    view/geometry/auto_layout.cpp
    view/shell/icons.cpp
    view/items/state_item.cpp
    view/items/transition_item.cpp
    view/items/handle_item.cpp
    view/items/machine_frame_item.cpp
    view/items/note_item.cpp
    view/items/action_box.cpp
    view/items/code_lens_item.cpp
    view/canvas/canvas_view.cpp
    view/canvas/canvas_interaction_fsm.cpp
    view/canvas/canvas_presenter.cpp
    view/canvas/canvas_presenter_gestures.cpp
    view/canvas/canvas_presenter_verbs.cpp
    view/canvas/item_registry_delegate.cpp
    view/canvas/routing_delegate.cpp
    view/canvas/selection_delegate.cpp
    view/canvas/drag_session_delegate.cpp
    view/canvas/wire_drag_delegate.cpp
    view/canvas/sim_presentation.cpp
    view/canvas/inline_edit_controller.cpp
    view/canvas/note_editor_fsm.cpp
    view/canvas/note_editor_rules.cpp
    view/canvas/canvas_menus.cpp
    view/geometry/node_anchors.cpp
    view/shell/theme.cpp
    view/shell/document_session.cpp
    view/shell/editor_view.cpp
    view/shell/minimap_view.cpp
    view/shell/editor_group.cpp
    view/shell/floating_window.cpp
    view/shell/machines_panel.cpp
    view/shell/logic_panel.cpp
    view/shell/logic_panel_types.cpp
    view/shell/logic_adapter.cpp
    view/shell/struct_editor_dialog.cpp
    view/shell/expression_editor_dialog.cpp
    view/shell/export_dialog.cpp
    view/shell/auto_layout_dialog.cpp
    view/shell/inspector_panel.cpp
    view/shell/inspector_adapter.cpp
    view/shell/node_code_preview_widget.cpp
    view/shell/trace_panel.cpp
    view/shell/settings_view.cpp
    view/shell/progress_overlay_widget.cpp
    view/shell/main_window.cpp
)

# Windows exe icon. resources/app/state-designer.ico is built from the brand
# mark by tools/app-icon/make_ico.py.
if(WIN32)
    target_sources(state-designer PRIVATE resources/app/state-designer.rc)
endif()

target_include_directories(state-designer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The exe must land in <build>/, not <build>/src/: probeCaptureDir() resolves
# captures as applicationDirPath() + "/../temp/...", one level below the repo
# root.
set_target_properties(state-designer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

target_link_libraries(state-designer
    PRIVATE
        ordo::qt
        Qt6::Widgets
        Qt6::Network
        Qt6::Svg
)

if(ENABLE_LOGGING)
    target_compile_definitions(state-designer PRIVATE SD_ENABLE_LOGGING=1)
else()
    target_compile_definitions(state-designer PRIVATE SD_ENABLE_LOGGING=0 QT_NO_DEBUG_OUTPUT)
endif()

if(ENABLE_DEV_DEMO)
    target_compile_definitions(state-designer PRIVATE SD_ENABLE_DEV_DEMO=1)
else()
    target_compile_definitions(state-designer PRIVATE SD_ENABLE_DEV_DEMO=0)
endif()

# Color literals are not allowed outside src/constants/: every color is a role
# token from constants/design_tokens.h. When tools/color-literal-check is
# present, the check runs before every build of the exe, and a missing Python
# fails the configure rather than silently dropping the check; opt out
# explicitly with -DSD_SKIP_COLOR_GATE=ON. A tree without the script builds
# without the check.
option(SD_SKIP_COLOR_GATE "Build without the color-literal gate (explicit opt-out only)" OFF)
set(_sd_color_check "${CMAKE_SOURCE_DIR}/tools/color-literal-check/check.py")
if(SD_SKIP_COLOR_GATE)
    message(WARNING "SD_SKIP_COLOR_GATE=ON: the color-literal gate is OFF for this build tree")
elseif(NOT EXISTS "${_sd_color_check}")
    message(STATUS "state-designer: tools/color-literal-check not present, building without the color-literal gate")
else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND AND WIN32)
        # Shim-based installs (pyenv-win) put only a .bat on PATH, which
        # find_package cannot see; ask the shell's `python` where it really lives,
        # then fall back to pyenv-win's own versions directory, which does not
        # depend on the configuring shell's PATH at all.
        execute_process(COMMAND cmd /c python -c "import sys; print(sys.executable)"
                        OUTPUT_VARIABLE _sd_python OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT (_sd_python AND EXISTS "${_sd_python}"))
            file(GLOB _sd_pyenv_pythons "$ENV{USERPROFILE}/.pyenv/pyenv-win/versions/*/python.exe")
            if(_sd_pyenv_pythons)
                list(SORT _sd_pyenv_pythons)
                list(GET _sd_pyenv_pythons -1 _sd_python)
            endif()
        endif()
        if(_sd_python AND EXISTS "${_sd_python}")
            set(Python3_EXECUTABLE "${_sd_python}")
            set(Python3_Interpreter_FOUND TRUE)
        endif()
    endif()
    if(Python3_Interpreter_FOUND)
        add_custom_target(color-literal-check
            COMMAND ${Python3_EXECUTABLE} ${_sd_color_check} ${CMAKE_CURRENT_SOURCE_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking src/ for color literals"
            VERBATIM)
        add_dependencies(state-designer color-literal-check)
    else()
        message(FATAL_ERROR "Python3 not found, so the color-literal gate (tools/color-literal-check) cannot run. "
                            "Put python on PATH (or install it), or configure with -DSD_SKIP_COLOR_GATE=ON to "
                            "build without the gate explicitly.")
    endif()
endif()

# dbghelp (StackWalk64) for infra/crash_handler.cpp.
if(WIN32)
    target_link_libraries(state-designer PRIVATE dbghelp)
endif()
