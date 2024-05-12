add_library(ASIO INTERFACE)
target_include_directories(ASIO INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/asio/include)
# Use as standalone library and do not allow deprecated features
target_compile_definitions(ASIO INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)