## Applies patches/sourcepp-vtx-bounds-check.patch to the fetched sourcepp source,
## unless it is already applied (so re-running CMake configure against an
## already-patched _deps checkout doesn't fail the build).
##
## Expected variables (set via FetchContent's PATCH_COMMAND):
##   GIT_EXECUTABLE   - path to git
##   SOURCE_DIR       - the fetched sourcepp source directory (checked out repo root)
##   PATCH_FILE       - path to the patch file to apply

if(NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE)
	message(FATAL_ERROR "apply-sourcepp-patches.cmake: missing required variable(s) (GIT_EXECUTABLE/SOURCE_DIR/PATCH_FILE)")
endif()

# --check --reverse succeeds (exit 0) only if the patch is already fully applied.
execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --whitespace=nowarn --ignore-whitespace "${PATCH_FILE}"
	WORKING_DIRECTORY "${SOURCE_DIR}"
	RESULT_VARIABLE already_applied_result
	OUTPUT_QUIET
	ERROR_QUIET
)

if(already_applied_result EQUAL 0)
	message(STATUS "sourcepp: bounds-check patch already applied, skipping.")
	return()
endif()

execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn --ignore-whitespace "${PATCH_FILE}"
	WORKING_DIRECTORY "${SOURCE_DIR}"
	RESULT_VARIABLE apply_result
)

if(NOT apply_result EQUAL 0)
	message(FATAL_ERROR
		"Failed to apply ${PATCH_FILE} to sourcepp source at ${SOURCE_DIR}.\n"
		"This usually means the pinned sourcepp GIT_TAG has drifted from what the "
		"patch's line-context expects. See patches/README.md for how to resolve "
		"(re-validate/refresh the patch, or drop it if upstream fixed the issue).")
endif()

message(STATUS "sourcepp: bounds-check patch applied successfully.")
