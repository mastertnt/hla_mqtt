@echo off
rem Build complet Release + Debug avec Conan 1.66 et Visual Studio 2019.
rem Prérequis : profil Conan "default" (conan profile new default --detect),
rem             cmake >= 3.20 dans le PATH.
setlocal
cd /d "%~dp0"

rem CMake 4.x : accepte les dependances declarant cmake_minimum_required < 3.5
set CMAKE_POLICY_VERSION_MINIMUM=3.5

conan install . -pr:h profiles\conan1\vs2019-release -pr:b default --build=missing || exit /b 1
conan install . -pr:h profiles\conan1\vs2019-debug   -pr:b default --build=missing || exit /b 1

cmake -S . -B build -G "Visual Studio 16 2019" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake || exit /b 1

cmake --build build --config Release || exit /b 1
cmake --build build --config Debug   || exit /b 1

echo.
echo Sorties : build\bin\Release\rti1516e.dll  build\bin\Debug\rti1516ed.dll
echo Test    : build\bin\Release\hla-test-federate.exe --selftest
