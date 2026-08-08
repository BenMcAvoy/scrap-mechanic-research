set_project("steam_lobby_probe")
set_version("0.1.0")
set_languages("c++20")
set_toolchains("clang-cl")

target("steam_lobby_probe")
    set_kind("binary")
    add_files("src/main.cpp")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
    add_cxxflags("/EHsc", "/W4", "/permissive-")
    add_ldflags("/SUBSYSTEM:CONSOLE")
    add_syslinks("kernel32")

