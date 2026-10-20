# Build script for exchange_node (MinGW g++, C++20)
$gpp = "C:\msys64\mingw64\bin\g++.exe"
$inc = "-I."
$std  = "-std=c++20"

Write-Host "Compiling main.cpp" -ForegroundColor Yellow
& $gpp $std $inc -c main.cpp

Write-Host "Compiling socket_reader.cpp" -ForegroundColor Yellow
& $gpp $std $inc -c socket_reader.cpp

Write-Host "Compiling verilator_hooked.h (header-only)" -ForegroundColor Yellow
# No compile needed for header-only file

Write-Host "DONE" -ForegroundColor Green

