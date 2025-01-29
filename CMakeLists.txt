cmake_minimum_required(VERSION 3.0)
project(corral)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(corral INTERFACE
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

option(CORRAL_TEST_RUNNER "Use this wrapper for running tests" "")
option(CORRAL_XBUILD "Do not pull any features unavailable while cross-compiling" "")

option(
    CORRAL_CATCH2
    "include path for Catch2 (or Git repository URL)"
    "https://github.com/catchorg/catch2"
)

if("${CORRAL_CATCH2}" MATCHES ".*://.*")
    include(FetchContent)
    FetchContent_Declare(
        catch
        GIT_REPOSITORY "${CORRAL_CATCH2}"
        GIT_TAG v2.13.9
    )
    FetchContent_MakeAvailable(catch)
else()
    set(Catch_INCLUDE_DIR "${CORRAL2_CATCH}")
    find_package(Catch2 2.13.9 REQUIRED)
endif()

enable_testing()

add_executable(corral_basic_test test/basic_test.cc test/helpers.h)
target_link_libraries(corral_basic_test PRIVATE corral Catch2::Catch2)
target_compile_definitions(corral_basic_test PRIVATE CATCH_CONFIG_MAIN)
add_test(NAME corral_basic_test COMMAND ${CORRAL_TEST_RUNNER} corral_basic_test)


#
# ASIO interaction
#

option(CORRAL_BOOST "include path for boost (or Git repository URL)" "")

if(NOT ("${CORRAL_BOOST}" STREQUAL ""))
    if("${CORRAL_BOOST}" MATCHES ".*://.*")
        FetchContent_Declare(
            Boost
            GIT_REPOSITORY "${CORRAL_BOOST}"
            GIT_TAG boost-1.80.0
        )
        FetchContent_MakeAvailable(Boost)
    else()
        set(Boost_INCLUDE_DIR "${CORRAL_BOOST}")
        find_package(Boost 1.77.0 REQUIRED)
    endif()

    add_executable(corral_asio_test test/asio_test.cc test/helpers.h)
    target_link_libraries(corral_asio_test PRIVATE corral Catch2::Catch2)
    target_compile_definitions(corral_asio_test PRIVATE CATCH_CONFIG_MAIN)

    if("${CORRAL_TEST_RUNNER}" STREQUAL "")
        find_package(OpenSSL)
        if(OpenSSL_FOUND)
            target_compile_definitions(corral_asio_test PRIVATE CORRAL_HAVE_OPENSSL)
            target_link_libraries(corral_asio_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()

    add_test(NAME corral_asio_test COMMAND ${CORRAL_TEST_RUNNER} corral_asio_test)
endif()


#
# Installation
#

install(DIRECTORY corral DESTINATION include)
install(TARGETS corral EXPORT corralTargets
    INCLUDES DESTINATION include
)


#
# Examples
#
option(CORRAL_EXAMPLES "Build examples" OFF)
if(CORRAL_EXAMPLES)
    add_subdirectory(examples)
endif()
