@echo off
REM Unit Test Runner for SRRHI Project
REM This script runs all validation tests and captures output

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Set project root
set PROJECT_ROOT=%cd%

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
cmake --build "%PROJECT_ROOT%\build" --target ALL_BUILD >> "%output_file%" 2>&1
echo Exit code: !errorlevel! >> "%output_file%"
echo ALL_BUILD completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 2: Run srrhi.exe to regenerate headers (now includes static_assert checks)
echo Step 2: Running srrhi.exe with test input... >> "%output_file%"
echo Command: %PROJECT_ROOT%\bin\srrhi.exe -i %PROJECT_ROOT%\test\input -o %PROJECT_ROOT%\test\output --test >> "%output_file%"
echo. >> "%output_file%"
"%PROJECT_ROOT%\bin\srrhi.exe" -i "%PROJECT_ROOT%\test\input" -o "%PROJECT_ROOT%\test\output" --test >> "%output_file%"
echo Exit code: !errorlevel! >> "%output_file%"
echo srrhi.exe completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 3: Run Python validation generation (generates include-only .cpp stubs)
echo Step 3: Running generate_validation.py... >> "%output_file%"
echo. >> "%output_file%"
python "%PROJECT_ROOT%\generate_validation.py" >> "%output_file%" 2>&1
echo Exit code: !errorlevel! >> "%output_file%"
echo generate_validation.py completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 4: Run cmake configure to generate any new validation targets
echo Step 4: Running cmake configure... >> "%output_file%"
echo. >> "%output_file%"
cmake "%PROJECT_ROOT%" -B "%PROJECT_ROOT%\build" >> "%output_file%" 2>&1
echo Exit code: !errorlevel! >> "%output_file%"
echo cmake configure completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 5: Rebuild validation stubs that now include the updated headers
echo Step 5: Rebuilding validation stubs... >> "%output_file%"
echo. >> "%output_file%"
cmake --build "%PROJECT_ROOT%\build" --target ALL_BUILD >> "%output_file%" 2>&1
echo Exit code: !errorlevel! >> "%output_file%"
echo Rebuild completed >> "%output_file%"
echo. >> "%output_file%"

REM Step 6: Run all validation executables
echo Step 6: Running all validation executables... >> "%output_file%"
echo. >> "%output_file%"

for /f "delims=" %%f in ('dir /b "%PROJECT_ROOT%\bin\validation_*.exe"') do (
    echo Running: %%f >> "%output_file%"
    echo -------------------------------- >> "%output_file%"
    "%PROJECT_ROOT%\bin\%%f" >> "%output_file%" 2>&1
    echo Exit code: !errorlevel! >> "%output_file%"
    echo %%f completed >> "%output_file%"
    echo. >> "%output_file%"
)

REM Final summary
echo. >> "%output_file%"
echo ================================== >> "%output_file%"
set mydate2=%date:~10,4%%date:~4,2%%date:~7,2%
set mytime2=%time:~0,2%%time:~3,2%%time:~6,2%
set mytime2=%mytime2: =0%
echo Finished: %mydate2%_%mytime2% >> "%output_file%"
echo ================================== >> "%output_file%"

echo.
echo Test execution completed!
echo Results saved to: %output_file%
echo.
type "%output_file%"

endlocal
