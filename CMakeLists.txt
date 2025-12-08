cmake_minimum_required(VERSION 3.15)

set(THE_CORRAL_VERSION 1.0.0)

if(CORRAL_TEST_INSTALLED_VERSION)
  find_package(corral ${THE_CORRAL_VERSION} REQUIRED)

  message("corral_FOUND: ${corral_FOUND}")

  add_library(dummy_target INTERFACE)

  target_link_libraries(dummy_target INTERFACE corral::corral)

  find_package(PkgConfig)

  pkg_check_modules(corral GLOBAL IMPORTED_TARGET corral)

  add_library(dummy_target2 INTERFACE)

  target_link_libraries(dummy_target2 INTERFACE PkgConfig::corral)
  return()
endif()

project(corral VERSION ${THE_CORRAL_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(corral INTERFACE)
add_library(corral::corral ALIAS corral)

target_include_directories(
  corral INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                   $<INSTALL_INTERFACE:include>
)

option(SKIP_TESTS "Do not build corral unit tests.")
if(NOT ${SKIP_TESTS})

  set(CORRAL_TEST_RUNNER
      ""
      CACHE STRING "Use this wrapper for running tests"
  )

  set(CORRAL_CATCH2
      "https://github.com/catchorg/catch2"
      CACHE STRING "include path for Catch2 (or Git repository URL)"
  )

  if("${CORRAL_CATCH2}" MATCHES ".*://.*")
    message(
      STATUS
        "Trying to bundle Catch2 ver 2.xx.x"
    )
    include(FetchContent)
    FetchContent_Declare(
      catch
      GIT_REPOSITORY "${CORRAL_CATCH2}"
      GIT_TAG v2.13.9
    )
    FetchContent_MakeAvailable(catch)
  else()
    message(STATUS "Trying to use installed Catch2.")
    set(Catch_INCLUDE_DIR "${CORRAL_CATCH2}")
    find_package(Catch2 2.13.9 REQUIRED)
  endif()

  enable_testing()

  add_executable(
    corral_basic_test
    test/basic_test.cc
    test/adapter_test.cc
    test/callback_test.cc
    test/channel_test.cc
    test/combiner_test.cc
    test/error_policy_test.cc
    test/nursery_test.cc
    test/stack_trace_test.cc
    test/sync_primitive_test.cc
    test/config.h
    test/helpers.h
    test/TestEventLoop.h
  )
  target_link_libraries(corral_basic_test PRIVATE corral Catch2::Catch2)
  add_test(NAME corral_basic_test COMMAND ${CORRAL_TEST_RUNNER}
                                          corral_basic_test
  )

  #
  # ASIO interaction
  #

  set(CORRAL_BOOST
      ""
      CACHE STRING "include path for boost (or Git repository URL)"
  )

  if(NOT ("${CORRAL_BOOST}" STREQUAL ""))
    if("${CORRAL_BOOST}" MATCHES ".*://.*")
      message(
        STATUS "Trying to bundle Boost (maybe via git cloning whole Boost)."
      )
      FetchContent_Declare(
        Boost
        GIT_REPOSITORY "${CORRAL_BOOST}"
        GIT_TAG boost-1.80.0
      )
      FetchContent_MakeAvailable(Boost)
      set(CORRAL_BOOST_ASIO_LIB_NAME "Boost::asio")
    else()
      message(STATUS "Trying to use installed Boost.")
      set(Boost_INCLUDE_DIR "${CORRAL_BOOST}")
      find_package(Boost 1.77.0 REQUIRED)
      set(CORRAL_BOOST_ASIO_LIB_NAME "Boost::boost")
    endif()

    add_executable(corral_asio_test test/asio_test.cc test/config.h test/config.h)
    target_link_libraries(corral_asio_test PRIVATE corral Catch2::Catch2)

    if("${CORRAL_TEST_RUNNER}" STREQUAL "")
      message(STATUS "Trying to use installed OpenSSL.")
      find_package(OpenSSL)
      if(OpenSSL_FOUND)
        message(STATUS "Using installed OpenSSL.")
        target_compile_definitions(corral_asio_test PRIVATE CORRAL_HAVE_OPENSSL)
        target_link_libraries(
          corral_asio_test PRIVATE OpenSSL::SSL OpenSSL::Crypto
        )
      else()
        message(STATUS "Failed to find installed OpenSSL")
      endif()
    endif()

    add_test(NAME corral_asio_test COMMAND ${CORRAL_TEST_RUNNER}
                                           corral_asio_test
    )
  endif()

endif()

#
# Installation
#

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/${PROJECT_NAME}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  TARGETS ${PROJECT_NAME}
  EXPORT ${PROJECT_NAME}Targets
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  EXPORT ${PROJECT_NAME}Targets
  DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/${PROJECT_NAME}/
  NAMESPACE ${PROJECT_NAME}::
)

file(WRITE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake.in"
  "@PACKAGE_INIT@\ninclude(\$\{CMAKE_CURRENT_LIST_DIR\}/${PROJECT_NAME}Targets.cmake)"
)

configure_package_config_file(
  ${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake.in
  ${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/${PROJECT_NAME}
)

install(FILES "${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/${PROJECT_NAME}
)

write_basic_package_version_file(
  ${CMAKE_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion ARCH_INDEPENDENT
)

install(FILES "${CMAKE_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/${PROJECT_NAME}
)

file(
  GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.pc"
  CONTENT
    "prefix=${CMAKE_INSTALL_PREFIX}\nincludedir=\$\{prefix\}/include\nName: ${PROJECT_NAME}\nDescription: Lightweight structured concurrency for C++20\nVersion: ${PROJECT_VERSION}\nCflags: -I\$\{includedir\}\n"
)

install(FILES "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.pc"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pkgconfig
)

#
# Examples
#
option(SKIP_EXAMPLES "Do not build corral examples")
if (NOT ${SKIP_EXAMPLES})
  add_subdirectory(examples)
endif()
