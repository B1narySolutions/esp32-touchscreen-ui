idf_component_register(
    SRCS
        "app_main.c"
        "board_init.c"
        "io_expander.c"
        "diag.c"
        "seed_link.c"
        "seed_link_proto/seed_link_proto.c"
        "seed_link_proto/slp_coalesce.c"
        "rig/rig_state.c"
        "rig/amp_models.c"
        "knobs/knobs.c"
        "ui/ui_main.c"
        "ui/ui_effects_data.c"
        "ui/ui_effect_chip.c"
        "ui/ui_knob.c"
        "ui/ui_ir_curve.c"
        "ui/ui_vertical_slider.c"
        "ui/ui_test_mode.c"
    INCLUDE_DIRS
        "."
        "ui"
        "rig"
        "seed_link_proto"
        "knobs"
)

# Fail the build if the wire parameter registry (tools/param_ids.json) and the knobs defined in
# ui_effects_data.c disagree, or if the generated headers are stale. See tools/gen_params.py.
idf_build_get_property(python PYTHON)
set(repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
set(param_stamp "${CMAKE_CURRENT_BINARY_DIR}/param_map_check.stamp")
add_custom_command(
    OUTPUT "${param_stamp}"
    COMMAND "${python}" "${repo_root}/tools/gen_params.py" --check
    COMMAND "${CMAKE_COMMAND}" -E touch "${param_stamp}"
    DEPENDS
        "${repo_root}/tools/gen_params.py"
        "${repo_root}/tools/param_ids.json"
        "${CMAKE_CURRENT_LIST_DIR}/ui/ui_effects_data.c"
        "${CMAKE_CURRENT_LIST_DIR}/ui/ui_effects_data.h"
        "${CMAKE_CURRENT_LIST_DIR}/rig/param_map_gen.h"
        "${CMAKE_CURRENT_LIST_DIR}/seed_link_proto/slp_params.h"
        "${repo_root}/docs/SEED_LINK_PROTOCOL.md"
    COMMENT "Checking the Seed link parameter map"
    VERBATIM)
add_custom_target(param_map_check DEPENDS "${param_stamp}")
add_dependencies(${COMPONENT_LIB} param_map_check)
