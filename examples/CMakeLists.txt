
add_executable(asio_hello_world asio_hello_world.cc)
target_link_libraries(asio_hello_world PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}")

add_executable(asio_echo_server asio_echo_server.cc)
target_link_libraries(asio_echo_server PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}")

add_executable(asio_netcat asio_netcat.cc)
target_link_libraries(asio_netcat PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}")

add_executable(asio_http_downloader asio_http_downloader.cc)
target_link_libraries(asio_http_downloader PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}")

add_executable(asio_happy_eyeballs asio_happy_eyeballs.cc)
target_link_libraries(asio_happy_eyeballs PRIVATE corral "${CORRAL_BOOST_ASIO_LIB_NAME}")


find_package(Qt6 COMPONENTS Core Network)

if(${Qt6Core_FOUND})
  qt_add_executable(qt_echo_server qt_echo_server.cc)
  target_link_libraries(qt_echo_server PRIVATE corral Qt6::Core Qt6::Network)
endif()

