@echo off
REM Unit Test Runner for SRRHI Project
REM This script runs all validation tests and captures output

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Set project root
set PROJECT_ROOT=%cd%

REM Track test failures
set FAILED_TESTS=
set BUILD_FAILED=0

REM Create output file with timestamp (ISO 8601: YYYYMMDD_HHMMSS for correct lexicographic sort)
set mydate=%date:~10,4%%date:~4,2%%date:~7,2%
set mytime=%time:~0,2%%time:~3,2%%time:~6,2%
set mytime=%mytime: =0%
set output_file=%PROJECT_ROOT%\bin\test_results_%mydate%_%mytime%.txt

echo ================================== >> "%output_file%"
echo SRRHI Unit Test Execution >> "%output_file%"
echo Started: %mydate%_%mytime% >> "%output_file%"
echo ================================== >> "%output_file%"
echo. >> "%output_file%"

REM Step 1: Build srrhi.exe (and validation stubs) from source
echo Step 1: Building ALL_BUILD target... >> "%output_file%"
echo. >> "%output_file%"
cmake --build "%PROJECT_ROOT%\build" --config Release --target ALL_BUILD --parallel >> "%output_file%" 2>&1
set BUILD_RESULT=!errorlevel!
echo Exit code: !BUILD_RESULT! >> "%output_file%"
if !BUILD_RESULT! neq 0 set BUILD_FAILED=1
echo ALL_BUILD completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 2: Run srrhi.exe to regenerate headers, generate validation stubs, and run reflection tests
echo Step 2: Running srrhi.exe with test input (--test --gen-validation)... >> "%output_file%"
echo Command: %PROJECT_ROOT%\bin\srrhi.exe -i %PROJECT_ROOT%\test\input -o %PROJECT_ROOT%\test\output --test --gen-validation >> "%output_file%"
echo. >> "%output_file%"
"%PROJECT_ROOT%\bin\srrhi.exe" -i "%PROJECT_ROOT%\test\input" -o "%PROJECT_ROOT%\test\output" --test --gen-validation >> "%output_file%"
set SRRHI_RESULT=!errorlevel!
echo Exit code: !SRRHI_RESULT! >> "%output_file%"
if !SRRHI_RESULT! neq 0 set BUILD_FAILED=1
echo srrhi.exe completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 3: Run cmake configure to generate any new validation targets
echo Step 3: Running cmake configure... >> "%output_file%"
echo. >> "%output_file%"
cmake "%PROJECT_ROOT%" -B "%PROJECT_ROOT%\build" >> "%output_file%" 2>&1
echo Exit code: !errorlevel! >> "%output_file%"
echo cmake configure completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 4: Rebuild validation stubs that now include the updated headers
echo Step 4: Rebuilding validation stubs... >> "%output_file%"
echo. >> "%output_file%"
cmake --build "%PROJECT_ROOT%\build" --config Release --target ALL_BUILD --parallel >> "%output_file%" 2>&1
echo Exit code: !errorlevel! >> "%output_file%"
echo Rebuild completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 5: Run all validation executables
echo Step 5: Running all validation executables... >> "%output_file%"
echo. >> "%output_file%"

for /f "delims=" %%f in ('dir /b "%PROJECT_ROOT%\bin\validation_*.exe"') do (
    echo Running: %%f >> "%output_file%"
    echo -------------------------------- >> "%output_file%"
    "%PROJECT_ROOT%\bin\%%f" >> "%output_file%" 2>&1
    set TEST_RESULT=!errorlevel!
    echo Exit code: !TEST_RESULT! >> "%output_file%"
    if !TEST_RESULT! neq 0 (
        set FAILED_TESTS=!FAILED_TESTS!  - %%f^
!
    )
    echo %%f completed >> "%output_file%"
    echo. >> "%output_file%"
)

REM Step 6: Run HLSL runtime validation (D3D12 + DXC shader execution tests)
echo Step 6: Running hlsl_runtime_validation.exe... >> "%output_file%"
echo. >> "%output_file%"
echo Running: hlsl_runtime_validation.exe >> "%output_file%"
echo -------------------------------- >> "%output_file%"
"%PROJECT_ROOT%\bin\hlsl_runtime_validation.exe" >> "%output_file%" 2>&1
set HLSL_RUNTIME_RESULT=!errorlevel!
echo Exit code: !HLSL_RUNTIME_RESULT! >> "%output_file%"
if !HLSL_RUNTIME_RESULT! neq 0 set FAILED_TESTS=!FAILED_TESTS!  - hlsl_runtime_validation.exe^
echo hlsl_runtime_validation.exe completed >> "%output_file%"
echo. >> "%output_file%"

REM Final summary
echo. >> "%output_file%"
echo ================================== >> "%output_file%"
set mydate2=%date:~10,4%%date:~4,2%%date:~7,2%
set mytime2=%time:~0,2%%time:~3,2%%time:~6,2%
set mytime2=%mytime2: =0%
echo Finished: %mydate2%_%mytime2% >> "%output_file%"
echo. >> "%output_file%"

REM Print test summary
if !BUILD_FAILED! equ 1 (
    echo TEST SUMMARY: Build or srrhi.exe failed >> "%output_file%"
) else if defined FAILED_TESTS (
    echo TEST SUMMARY: Some validation tests FAILED: >> "%output_file%"
    echo !FAILED_TESTS! >> "%output_file%"
) else (
    echo TEST SUMMARY: All tests PASSED >> "%output_file%"
)
echo ================================== >> "%output_file%"

echo.
echo Test execution completed!
echo Results saved to: %output_file%
echo.
type "%output_file%"

REM Print short summary to console as final line
echo.
if !BUILD_FAILED! equ 1 (
    echo TEST SUMMARY: Build or srrhi.exe failed
) else if defined FAILED_TESTS (
    echo TEST SUMMARY: Some validation tests FAILED:
    echo !FAILED_TESTS!
) else (
    echo TEST SUMMARY: All tests PASSED
)

endlocal
