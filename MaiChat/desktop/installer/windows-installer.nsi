; MaiChat Windows 安装程序（NSIS）。
; 由 scripts\make-installer-windows.ps1 调用，需要以下命令行定义：
;   /DSTAGING_DIR=<staging 目录>   打包好的应用文件（package-windows.ps1 产出）
;   /DOUT_FILE=<输出 exe 路径>
;   /DAPP_VERSION=<显示版本，如 2026.07.18-abc1234>
;   /DAPP_VERSION_NUM=<数字版本，如 2026.7.18.0>
;   /DICON_FILE=<安装程序图标 .ico>
;
; 设计要点：
; - 按机器安装（需要管理员/UAC），装到 C:\Program Files\MaiChat
; - 卸载保留用户数据（%APPDATA%\MaiChat\Desktop IM 下的聊天库与设置）
; - 安装/卸载前结束运行中的实例，避免文件占用
; - 安装时清理 0.1.21 及更早版本留下的按用户安装（见 CleanupLegacyPerUserInstall）

Unicode true
SetCompressor /SOLID lzma
; 安装器自身声明 DPI-aware：高分屏下安装界面不发虚，也减少系统对
; 未签名安装程序的兼容性 shim 干预。
ManifestDPIAware true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

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
; 装进 Program Files 属于机器范围写入，必须提权，否则 File 会全部失败。
RequestExecutionLevel admin
; 应用是 64 位（build-msvc2019_64），但 makensis 产出的安装器本身是 32 位，
; $PROGRAMFILES 会给出 "Program Files (x86)"，所以这里必须用 $PROGRAMFILES64。
InstallDir "$PROGRAMFILES64\${APP_NAME}"
; 这里不用 InstallDirRegKey：它在 .onInit 之前就被处理，读不到 SetRegView 64
; 设定的注册表视图，覆盖安装时会读空而退回默认目录。改为在 .onInit 里手读。

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
; 「立即运行」必须经 explorer.exe 转启：安装器进程可能被 SmartScreen/PCA
; 套上兼容层（如 DPIUNAWARE，从浏览器下载的未签名安装包尤其常见），直接
; Exec 的子进程会继承 __COMPAT_LAYER，导致应用整窗按 100% 渲染再被系统
; 位图拉伸——全 UI 文字锯齿。经 explorer 转启后父进程与环境都是干净的。
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "立即运行 ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION RunAppViaExplorer
!insertmacro MUI_PAGE_FINISH

Function RunAppViaExplorer
  ; 经 explorer 转启还有一层好处：安装器现在是提权进程，直接 Exec 会让应用
  ; 也跑在管理员权限下（拖拽收文件等就会失效）。explorer 会把请求交给已在跑
  ; 的普通权限实例，应用回到正常用户权限。
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${APP_EXE}"'
FunctionEnd

Function .onInit
  SetRegView 64
  ; 覆盖安装沿用上次选择的目录（等价于原来的 InstallDirRegKey）。
  ReadRegStr $0 HKLM "${UNINST_KEY}" "InstallLocation"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

; 0.1.21 及更早版本按用户装在 %LOCALAPPDATA%\Programs\MaiChat。换到 Program Files
; 之后两份会并存：控制面板里两个 MaiChat、桌面两个快捷方式，还可能误启动旧版。
; 这里顺手把旧的按用户安装删掉（用户数据在 %APPDATA%，不受影响）。
; 注意：提权后 $LOCALAPPDATA / HKCU 指向的是通过 UAC 的那个账户。如果管理员和
; 当前登录用户不是同一人，这里就找不到旧目录——那种情况保持原样，不去猜别人的
; 用户目录，免得误删。
Function CleanupLegacyPerUserInstall
  ; $LOCALAPPDATA 受 SetShellVarContext 影响（all 上下文下会变成 ProgramData），
  ; 取旧目录前必须切回 current。
  SetShellVarContext current
  StrCpy $R0 "$LOCALAPPDATA\Programs\${APP_NAME}"
  IfFileExists "$R0\${APP_EXE}" 0 legacy_done
  ; 用户在目录页手动选回了老路径 —— 那是本次要装进去的目录，不能删。
  StrCmp $R0 "$INSTDIR" legacy_done 0
  DetailPrint "清理旧的按用户安装：$R0"
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  RMDir /r "$R0"
  DeleteRegKey HKCU "${UNINST_KEY}"
legacy_done:
  SetShellVarContext all
FunctionEnd

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

Section "安装"
  ; 机器范围安装：卸载信息写 HKLM，快捷方式建到全体用户。
  SetRegView 64
  SetShellVarContext all

  ; 结束运行中的实例，否则覆盖文件会失败（忽略未运行时的报错）。
  ; 用 nsExec::Exec 而不是 ExecWait：后者会给 taskkill 开一个真实的控制台窗口，
  ; 安装过程中黑窗一闪而过，看起来像装了什么可疑东西。nsExec 静默执行，
  ; 返回码压栈——这里不关心杀没杀到（没运行时本来就该失败），直接丢弃。
  nsExec::Exec 'taskkill /F /IM ${APP_EXE}'
  Pop $0

  Call CleanupLegacyPerUserInstall

  SetOutPath "$INSTDIR"
  File /r "${STAGING_DIR}\*.*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Kongshang"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"

  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all

  ; 同上：静默结束进程，不弹控制台黑窗。
  nsExec::Exec 'taskkill /F /IM ${APP_EXE}'
  Pop $0

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
  ; 用户数据（%APPDATA%\MaiChat\Desktop IM）与 Qt 设置保留，不随卸载删除。
SectionEnd
