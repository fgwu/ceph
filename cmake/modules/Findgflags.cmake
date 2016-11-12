# Try to find gflags
#
# Once done, this will define
#
# GFLAGS_FOUND
# GFLAGS_INCLUDE_DIR
# GFLAGS_LIBRARIES

find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h)
find_library(GFLAGS_LIBRARIES gflags)


mark_as_advanced(GFLAGS_INCLUDE_DIR GFLAGS_LIBRARIES)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(gflags DEFAULT_MSG
  GFLAGS_INCLUDE_DIR GFLAGS_LIBRARIES)
