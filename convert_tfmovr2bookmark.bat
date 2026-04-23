@echo off
setlocal enabledelayedexpansion

:: Check if a file was dragged and dropped
if "%~1"=="" (
    echo Error: Please drag and drop a .tfmovr file onto this batch file.
    pause
    exit /b
)

:: Set input and output paths based on the source file
set "inputFile=%~1"
set "outputFile=%~dpn1.bookmark.log"
set "tempFile=%temp%\tfm_processing.tmp"

:: Check if input file exists
if not exist "!inputFile!" (
    echo Error: Input file not found: "!inputFile!"
    exit /b
)

:: Check if output file exists and ask for overwrite
if exist "!outputFile!" (
    set /p "choice=Output file already exists. Overwrite? [y/N]: "
    if /i "!choice!" neq "y" (
        echo Operation cancelled by user.
        exit /b
    )
)

echo Processing: "!inputFile!"
echo Target: "!outputFile!"

if exist "%tempFile%" del "%tempFile%"

set /a count=0

:: Read .tfmovr and generate Trim lines
:: Patterns mapped based on source material indices
for /f "usebackq tokens=1,2,3 delims=, " %%a in ("%inputFile%") do (
    set /a count+=1
    set "start=%%a"

    echo !start!>> "%tempFile%"
)

:: Create the final .se file
type "%tempFile%"> "%outputFile%"

:: Cleanup and success message
if exist "%tempFile%" del "%tempFile%"
echo.
echo Conversion completed successfully!
echo Output: "!outputFile!"
