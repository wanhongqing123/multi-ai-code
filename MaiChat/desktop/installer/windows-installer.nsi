; MaiChat Windows 安装程序（NSIS）。
; 由 scripts\make-installer-windows.ps1 调用，需要以下命令行定义：
;   /DSTAGING_DIR=<staging 目录>   打包好的应用文件（package-windows.ps1 产出）
;   /DOUT_FILE=<输出 exe 路径>
;   /DAPP_VERSION=<显示版本，如 2026.07.18-abc1234>
;   /DAPP_VERSION_NUM=<数字版本，如 2026.7.18.0>
;   /DICON_FILE=<安装程序图标 .ico>
;
; 设计要点：
; - 按用户安装（无需管理员/UAC），装到 %LOCALAPPDATA%\Programs\MaiChat
; - 卸载保留用户数据（%APPDATA%\MaiChat\Desktop IM 下的聊天库与设置）
; - 安装/卸载前结束运行中的实例，避免文件占用

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef STAGING_DIR
  !error "STAGING_DIR 未定义"
!endif
!ifndef OUT_FILE
  !error "OUT_FILE 未定义"
!endif
!ifndef APP_VERSION
  !define APP_VERSION "0.0.0"
!endif
!ifndef APP_VERSION_NUM
  !define APP_VERSION_NUM "0.0.0.0"
!endif

!define APP_NAME "MaiChat"
!define APP_EXE "maichat.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MaiChat"

Name "${APP_NAME}"
OutFile "${OUT_FILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"

VIProductVersion "${APP_VERSION_NUM}"
VIAddVersionKey /LANG=2052 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=2052 "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${APP_VERSION}"

!ifdef ICON_FILE
  !define MUI_ICON "${ICON_FILE}"
  !define MUI_UNICON "${ICON_FILE}"
!endif

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "立即运行 ${APP_NAME}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

Section "安装"
  ; 结束运行中的实例，否则覆盖文件会失败（忽略未运行时的报错）。
  ExecWait 'taskkill /F /IM ${APP_EXE}' $0

  SetOutPath "$INSTDIR"
  File /r "${STAGING_DIR}\*.*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Kongshang"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Uninstall"
  ExecWait 'taskkill /F /IM ${APP_EXE}' $0

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINST_KEY}"
  ; 用户数据（%APPDATA%\MaiChat\Desktop IM）与 Qt 设置保留，不随卸载删除。
SectionEnd
