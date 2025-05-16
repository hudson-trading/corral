function(check_cxx_flags variable flag1)
  if(CMAKE_CXX_FLAGS MATCHES "${flag1}")
    message(DEBUG "CMAKE_CXX_FLAGS contains '${flag1}'")
    set(${variable}
        TRUE
        PARENT_SCOPE
    )
  else()
    message(DEBUG "CMAKE_CXX_FLAGS does not contain '${flag1}'")
    set(${variable}
        FALSE
        PARENT_SCOPE
    )
  endif()
endfunction()

# Example usage:
check_cxx_flags(CXX_FLAGS_NO_EXCEPTIONS "-fno-exceptions")
check_cxx_flags(CXX_FLAGS_NO_RTTI "-fno-rtti")

if(CXX_FLAGS_NO_EXCEPTIONS AND CXX_FLAGS_NO_RTTI)
  message(STATUS "CMAKE_CXX_FLAGS has both -fno-exceptions and -fno-rtti")
else()
  message(
    STATUS "CMAKE_CXX_FLAGS does not have both -fno-exceptions and -fno-rtti"
  )
endif()

if(NOT CXX_FLAGS_NO_EXCEPTIONS AND NOT CXX_FLAGS_NO_RTTI)
  add_executable(asio_hello_world asio_hello_world.cc)
  target_link_libraries(
    asio_hello_world PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}"
  )

  add_executable(asio_echo_server asio_echo_server.cc)
  target_link_libraries(
    asio_echo_server PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}"
  )

  add_executable(asio_http_downloader asio_http_downloader.cc)
  target_link_libraries(
    asio_http_downloader PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}"
  )

  add_executable(asio_happy_eyeballs asio_happy_eyeballs.cc)
  target_link_libraries(
    asio_happy_eyeballs PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}"
  )

  if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"))
    add_executable(asio_netcat asio_netcat.cc)
    target_link_libraries(
      asio_netcat PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}"
    )
  endif()

endif()

find_package(Qt6 COMPONENTS Core Network)

if(${Qt6Core_FOUND})
  qt_add_executable(qt_echo_server qt_echo_server.cc)
  target_link_libraries(qt_echo_server PRIVATE corral Qt6::Core Qt6::Network)
endif()
