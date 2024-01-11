
add_executable(asio_hello_world asio_hello_world.cc)
target_link_libraries(asio_hello_world PRIVATE corral Boost::asio)

add_executable(asio_echo_server asio_echo_server.cc)
target_link_libraries(asio_echo_server PRIVATE corral Boost::asio)

add_executable(asio_netcat asio_netcat.cc)
target_link_libraries(asio_netcat PRIVATE corral Boost::asio)

add_executable(asio_http_downloader asio_http_downloader.cc)
target_link_libraries(asio_http_downloader PRIVATE corral Boost::asio)

add_executable(asio_happy_eyeballs asio_happy_eyeballs.cc)
target_link_libraries(asio_happy_eyeballs PRIVATE corral Boost::asio)


find_package(Qt6 REQUIRED COMPONENTS Core Network)
qt_add_executable(qt_echo_server qt_echo_server.cc)
target_link_libraries(qt_echo_server PRIVATE corral Qt6::Core Qt6::Network)

