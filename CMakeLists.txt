cmake_minimum_required(VERSION 3.15)
project(corral)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
  corral
  INTERFACE
)
target_sources(corral
  INTERFACE
  corral/asio.h
  corral/CBPortal.h
  corral/Channel.h
  corral/concepts.h
  corral/config.h
  corral/defs.h
  corral/Event.h
  corral/Executor.h
  corral/corral.h
  corral/Nursery.h
  corral/ParkingLot.h
  corral/run.h
  corral/Semaphore.h
  corral/Shared.h
  corral/Task.h
  corral/ThreadPool.h
  corral/utility.h
  corral/Value.h
  corral/wait.h
  corral/detail/ABI.h
  corral/detail/exception.h
  corral/detail/frames.h
  corral/detail/introspect.h
  corral/detail/IntrusiveList.h
  corral/detail/IntrusivePtr.h
  corral/detail/ParkingLot.h
  corral/detail/platform.h
  corral/detail/PointerBits.h
  corral/detail/Promise.h
  corral/detail/Queue.h
  corral/detail/ScopeGuard.h
  corral/detail/TaskAwaiter.h
  corral/detail/utility.h
  corral/detail/wait.h
)

target_include_directories(corral INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

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

install(DIRECTORY corral DESTINATION include)
install(
  TARGETS corral
  EXPORT corralTargets
  INCLUDES
  DESTINATION include
)

#
# Examples
#
option(SKIP_EXAMPLES "Do not build corral examples")
if (NOT ${SKIP_EXAMPLES})
  add_subdirectory(examples)
endif()
