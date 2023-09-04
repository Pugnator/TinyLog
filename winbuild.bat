@echo off

REM Run CMake to generate build files
cmake -B build -G "MinGW Makefiles" -DDEBUG_ENABLE=0

REM Build the project
cmake --build build --clean-first