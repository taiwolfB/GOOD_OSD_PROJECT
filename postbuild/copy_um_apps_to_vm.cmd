@echo off

setlocal

pushd %CD%
cd %~dp0%

call paths.cmd

if [%VOL_MOUNT_LETTER%]==[__EMPTY__] goto fail
if [%PATH_TO_VM_DISK%]==[__EMPTY__] goto fail
if [%PATH_TO_VM_TOOLS%]==[__EMPTY__] goto fail


rem                                                               | Without this here there is an error: 
rem                                                               |   "The filename, directory name, or volume label syntax is incorrect."
rem                                                               | There should probably be a nicer way to do things...
rem                                                               v 
set PATH_TO_VM_MOUNT=%PATH_TO_VM_TOOLS:~0,-1%\bin\vmware-mount.exe"
set PATH_TO_BINARIES=%1
set FULL_PATH_TO_DESTINATION=%VOL_MOUNT_LETTER%\%2

echo "Path to VMWARE mount is [%PATH_TO_VM_MOUNT%]"
echo "Path to UM apps is [%PATH_TO_BINARIES%]"
echo "Full path to destination is [%FULL_PATH_TO_DESTINATION%]"

rem Mount the drive:
echo %PATH_TO_VM_MOUNT% %VOL_MOUNT_LETTER% %PATH_TO_VM_DISK%
%PATH_TO_VM_MOUNT% %VOL_MOUNT_LETTER% %PATH_TO_VM_DISK%

rem Copy the applications
xcopy /F /Y %PATH_TO_BINARIES%*.exe %FULL_PATH_TO_DESTINATION%

rem Unmount the drive:
%PATH_TO_VM_MOUNT% /d %VOL_MOUNT_LETTER%

goto end

:fail

echo "Failed, not all paths are valid!"
exit /b 1

:end

:: --- reload initial current directory ---
popd
exit /b %ERRORLEVEL%