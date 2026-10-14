cmake_minimum_required(VERSION 3.20)
project(kladde VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KLADDE_BUILD_TESTS "Build kladde-cpp's tests and tools" ${PROJECT_IS_TOP_LEVEL})
option(KLADDE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(KLADDE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(KLADDE_WARNINGS)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(KLADDE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Three compiled pieces and a header-only typed layer:
# - kladde_error: the error type everything shares (src/*.cpp);
# - kladde_schema: descriptors, canonical encoding, fingerprints (src/schema);
# - kladde_store: the untyped store (src/store);
# - kladde: all of them, plus the typed layer's headers.
file(GLOB KLADDE_ERROR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB KLADDE_SCHEMA_SOURCES CONFIGURE_DEPENDS src/schema/*.cpp)
file(GLOB KLADDE_STORE_SOURCES CONFIGURE_DEPENDS src/store/*.cpp)

add_library(kladde_error STATIC ${KLADDE_ERROR_SOURCES})
add_library(kladde_schema STATIC ${KLADDE_SCHEMA_SOURCES})
add_library(kladde_store STATIC ${KLADDE_STORE_SOURCES})
foreach(lib kladde_error kladde_schema kladde_store)
    target_include_directories(${lib} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(${lib} PUBLIC cxx_std_20)
    target_compile_options(${lib} PRIVATE ${KLADDE_WARNINGS})
    set_target_properties(${lib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()
target_link_libraries(kladde_schema PUBLIC kladde_error)
target_link_libraries(kladde_store PUBLIC kladde_error)

add_library(kladde INTERFACE)
add_library(kladde::kladde ALIAS kladde)
target_link_libraries(kladde INTERFACE kladde_schema kladde_store kladde_error)

if(KLADDE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    # One test executable per layer, so that each builds on its own.
    foreach(layer schema store typed)
        file(GLOB tests CONFIGURE_DEPENDS tests/${layer}/*.cpp)
        if(tests)
            add_executable(kladde-${layer}-tests ${tests})
            target_link_libraries(kladde-${layer}-tests PRIVATE kladde::kladde GTest::gtest GTest::gtest_main)
            target_compile_options(kladde-${layer}-tests PRIVATE ${KLADDE_WARNINGS})
            gtest_discover_tests(kladde-${layer}-tests)
        endif()
    endforeach()

    file(GLOB KLADDE_TOOLS CONFIGURE_DEPENDS tools/*.cpp)
    foreach(tool ${KLADDE_TOOLS})
        get_filename_component(name ${tool} NAME_WE)
        add_executable(${name} ${tool})
        target_link_libraries(${name} PRIVATE kladde::kladde)
        target_compile_options(${name} PRIVATE ${KLADDE_WARNINGS})
    endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS kladde kladde_error kladde_schema kladde_store EXPORT kladdeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT kladdeTargets NAMESPACE kladde:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kladde)
