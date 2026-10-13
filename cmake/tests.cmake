# Regression tests, wired so that `ctest` reproduces the release gate.
#
# Mirrors scripts/build_release_ci.ps1: the same executables and the same
# per-test argument matrix. Tests are custom [PASS]/[FAIL] console programs
# that return 0 on success, so no harness is needed beyond CTest itself.
#
# Tests are skipped when their target is not part of the configuration (several
# are gated behind LULING_WITH_SELFTEST or LULING_BUILD_PROGRESS_UI_TESTS), so a
# default build registers only what it can actually run.

function(luling_add_regression_test name target)
    if(NOT TARGET ${target})
        return()
    endif()
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 1800)
endfunction()

# Tests that run their own full suite with no arguments.
set(luling_plain_tests
    luling_rename_ops_test
    luling_child_edit_test
    luling_localization_test
    luling_update_test
    luling_update_installer_test
    luling_update_session_test
    luling_app_controllers_test
    luling_change_tracking_polling_test
    luling_change_tracking_test
    luling_index_delta_replay_test
    luling_runtime_log_test
    luling_diagnostics_export_test
    luling_shell_command_test
    luling_reparse_entry_test
    luling_link_destination_test
    luling_link_pill_test
    luling_shell_icons_test
    luling_network_locations_test
    luling_archive_integrity_test
    luling_document_completeness_test
    luling_preview_command_test
    luling_image_preview_integrity_test
    luling_udf_listing_test
    luling_preview_integrity_test
    luling_change_tracking_memory_test
    luling_change_feed_memory_test
    luling_usn_packet_queue_test
    luling_content_progress_ui_test
    luling_operation_presentation_test
    luling_column_strip_test
    luling_entry_sort_key_test
    luling_file_lock_test
    luling_dialogs_test)

foreach(test IN LISTS luling_plain_tests)
    luling_add_regression_test(${test} ${test})
endforeach()

# Tests whose default entry point is not the whole suite: each mode is a
# separate regression from the release gate, so register them individually.
luling_add_regression_test(ops_test.update_shutdown luling_ops_test --update-shutdown)
luling_add_regression_test(ops_test.delete_progress luling_ops_test --delete-progress-only)

luling_add_regression_test(playback_controls_test.timeline luling_playback_controls_test --timeline-only)
luling_add_regression_test(preview_test.vector luling_preview_test --vector-only)
luling_add_regression_test(preview_handler_probe.cooldown luling_preview_handler_probe --cooldown-test)

luling_add_regression_test(rename_ops_test.startup_stop luling_rename_ops_test --startup-stop)
luling_add_regression_test(rename_ops_test.shell_roundtrip luling_rename_ops_test --shell-roundtrip)

foreach(mode parent-cycle-only quiet-maintenance-only name-pool-only maintenance-only
             usn-only feed-only folder-sizes-only hierarchy-only visibility-cache-only)
    string(REPLACE "-" "_" suffix "${mode}")
    luling_add_regression_test(index_engine_test.${suffix} luling_index_engine_test --${mode})
    if(TARGET luling_index_engine_test)
        set_tests_properties(index_engine_test.${suffix} PROPERTIES
            ENVIRONMENT "LULING_SELFTEST_NO_SCREENSHOTS=1")
    endif()
endforeach()

foreach(mode layout-search-prefs layout-active-pane network-locations-view)
    string(REPLACE "-" "_" suffix "${mode}")
    luling_add_regression_test(app_controllers_test.${suffix} luling_app_controllers_test --${mode})
endforeach()

luling_add_regression_test(change_feed_memory_test.probe luling_change_feed_memory_test --probe)

foreach(mode service-start-only shutdown-only live-dedup-only folder-sizes-only)
    string(REPLACE "-" "_" suffix "${mode}")
    luling_add_regression_test(index_host_stress.${suffix} luling_index_host_stress --${mode})
endforeach()
