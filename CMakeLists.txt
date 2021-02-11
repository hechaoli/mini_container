cmake_minimum_required(VERSION 3.10)

project(MiniContainer)

find_package(Boost REQUIRED COMPONENTS program_options)

add_executable(mini_container mini_container.cpp)
target_link_libraries(mini_container PUBLIC Boost::program_options)
