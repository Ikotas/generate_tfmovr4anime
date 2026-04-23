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
set "outputFile=%~dpn1.se"
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
    set "end=%%b"
    set "pattern=%%c"
    
    set "idx="
    if "!pattern!"=="ccppc" set "idx=0,1,3,4"
    if "!pattern!"=="cppcc" set "idx=0,2,3,4"
    if "!pattern!"=="pcccp" set "idx=0,1,2,3"
    if "!pattern!"=="cccpp" set "idx=0,1,2,4"
    if "!pattern!"=="ppccc" set "idx=1,2,3,4"
    if "!pattern!"=="c"     set "idx=0,1,2,3"

    :: Default to 0,1,2,3 if pattern is unknown
    if "!idx!"=="" set "idx=0,1,2,3"

    echo v!count!=Trim^(!start!,!end!^).SelectEvery^(5,!idx!^)>> "%tempFile%"
)

:: Create the final .se file
type "%tempFile%"> "%outputFile%"

:: Generate concatenation string (blocks of 10 with backslashes)
set /a current=1
:concatLoop
set "line="
for /L %%i in (1,1,10) do (
    if !current! LEQ %count% (
        if "!line!"=="" (
            set "line=v!current!"
        ) else (
            set "line=!line!++v!current!"
        )
        set /a current+=1
    )
)

if !current! LEQ %count% (
    echo !line!++\>> "%outputFile%"
    goto concatLoop
) else (
    echo !line!>> "%outputFile%"
)

:: Cleanup and success message
if exist "%tempFile%" del "%tempFile%"
echo.
echo Conversion completed successfully!
echo Output: "!outputFile!"
