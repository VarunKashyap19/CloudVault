cmake_minimum_required(VERSION 3.15)

project(CloudVault)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SERVICE_COMPONENTS s3 logs)

find_package(AWSSDK REQUIRED COMPONENTS ${SERVICE_COMPONENTS})
find_package(OpenSSL REQUIRED)

add_executable(cloudvault
    src/main.cpp
)

target_link_libraries(cloudvault
    ${AWSSDK_LINK_LIBRARIES}
    OpenSSL::Crypto
)