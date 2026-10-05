cmake_minimum_required(VERSION 3.20)
project(Mini_LevelDB VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(MINI_LEVELDB_BUILD_BENCHMARK "Build the engine benchmark" ON)
option(MINI_LEVELDB_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(mini_leveldb db/mini_kv_store.cpp mem_table/mem_table.cpp)
add_library(mini_leveldb::mini_leveldb ALIAS mini_leveldb)
target_compile_features(mini_leveldb PUBLIC cxx_std_20)
set_target_properties(mini_leveldb PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(mini_leveldb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/mini_leveldb>)
target_link_libraries(mini_leveldb PUBLIC Threads::Threads)

if(MINI_LEVELDB_SANITIZERS)
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ASan/UBSan requires a supported GCC or Clang toolchain on Linux")
    endif()
    target_compile_options(mini_leveldb PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mini_leveldb PUBLIC -fsanitize=address,undefined)
endif()

add_executable(mini_leveldb_demo examples/storage_demo.cpp)
target_link_libraries(mini_leveldb_demo PRIVATE mini_leveldb::mini_leveldb)

if(MINI_LEVELDB_BUILD_BENCHMARK)
    add_executable(mini_kv_bench db/bench/mini_kv_bench.cpp)
    target_link_libraries(mini_kv_bench PRIVATE mini_leveldb::mini_leveldb)
endif()

if(BUILD_TESTING)
    add_executable(mini_leveldb_tests tests/engine_tests.cpp)
    target_link_libraries(mini_leveldb_tests PRIVATE mini_leveldb::mini_leveldb)
    foreach(case IN ITEMS crud tombstone reopen memtable_flush compaction multiple_versions
            concurrent_close wal_replay crash_recovery flush_crash_recovery truncated_wal
            reopen_versions empty_key all_deleted concurrent_compaction io_failure)
        add_test(NAME engine.${case} COMMAND mini_leveldb_tests ${case})
        set_tests_properties(engine.${case} PROPERTIES TIMEOUT 30)
    endforeach()
    set_tests_properties(engine.wal_replay engine.crash_recovery engine.flush_crash_recovery
        engine.truncated_wal engine.compaction engine.reopen_versions engine.concurrent_compaction
        PROPERTIES LABELS "integration")
endif()

install(TARGETS mini_leveldb EXPORT mini_leveldbTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY db/includes/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mini_leveldb/db/includes)
install(DIRECTORY mem_table/includes/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mini_leveldb/mem_table/includes)
configure_package_config_file(cmake/mini_leveldbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mini_leveldbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mini_leveldb)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/mini_leveldbConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(EXPORT mini_leveldbTargets NAMESPACE mini_leveldb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mini_leveldb)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mini_leveldbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mini_leveldbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mini_leveldb)
