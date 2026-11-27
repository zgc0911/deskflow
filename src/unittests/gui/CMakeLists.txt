# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME ServerConfigDialogTests
  DEPENDS gui
  SOURCE ServerConfigDialogTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
# the headers of the dialogs find one another from there
target_include_directories(ServerConfigDialogTests PRIVATE "${CMAKE_SOURCE_DIR}/src/lib/gui")
