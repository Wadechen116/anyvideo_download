# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "AnyVideoDownload_autogen"
  "CMakeFiles\\AnyVideoDownload_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\AnyVideoDownload_autogen.dir\\ParseCache.txt"
  )
endif()
