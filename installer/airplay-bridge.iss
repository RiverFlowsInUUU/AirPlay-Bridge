; AirPlay Bridge — 安装脚本（Inno Setup 6）
;
; 编译（在仓库根目录）：
;   ISCC.exe /DAppVersion=1.2.49 installer\airplay-bridge.iss
; 产物：
;   dist\AirPlay-Bridge-Setup-v1.2.49.exe
;
; 版本号由命令行传入（CI 从 CMakeLists.txt 读出来），
; 不在这里写死 —— 否则又多一处要手工同步的版本号。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#define AppName      "AirPlay Bridge"
#define AppPublisher "RiverFlowsInUUU"
#define AppURL       "https://github.com/RiverFlowsInUUU/AirPlay-Bridge"
#define ExeName      "AirPlayBridge.exe"

[Setup]
; AppId 用来识别「同一个程序的卸载记录」。**一旦发布就不能改** ——
; 改了会被当成另一个程序，控制面板里出现两条重复的安装项。
AppId={{B58BEA24-B1E5-4799-91D3-5DF384F6A7C0}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
DefaultDirName={autopf}\AirPlayBridge
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=AirPlay-Bridge-Setup-v{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#ExeName}
; 装到 Program Files 需要管理员提权。
; 注意：程序**运行**时不需要管理员 —— 配置和日志都在 %LOCALAPPDATA%，
; 只有加防火墙规则那一步会自己单独提权。
PrivilegesRequired=admin
; 升级或卸载时自动关掉正在跑的实例
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; ★ 装到磁盘上的文件名固定成 AirPlayBridge.exe，**不带版本号** ★
;
; 这是整个安装版的核心价值：开机自启项和防火墙规则都是**按 exe 路径**
; 写进系统的。文件名一变（免安装版每次下载都带版本号），那些记录就
; 全部失效、要重新注入。固定下来之后，以后升级装到同一个路径，
; 系统集成继续有效，用户什么都不用管。
Source: "..\build\Release\airplay_bridge.exe"; DestDir: "{app}"; DestName: "{#ExeName}"; Flags: ignoreversion
Source: "..\README.md";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";      DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\{#ExeName}"
Name: "{group}\卸载 {#AppName}";       Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\{#ExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
// 卸载开始时清掉「按 exe 路径」写进系统的那些东西（防火墙规则、开机自启项）。
//
// 为什么不用 [UninstallRun]：实测卸载阶段它的 waituntilterminated 不生效 ——
// 日志显示启动 cleanup 之后 130ms 就把 exe 删了，程序根本来不及跑完
//（清理要跑 COM 枚举 + netsh，约 1 秒）。结果就是防火墙规则留在系统里，
// 变成指向已删文件的死记录。
//
// 放在 InitializeUninstall（卸载最开头，文件都还在）里，
// 用 ewWaitUntilTerminated 明确等它结束。
function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
  Cleaner: String;
begin
  Result := True;
  Cleaner := ExpandConstant('{app}\{#ExeName}');
  if FileExists(Cleaner) then
    Exec(Cleaner, '--uninstall-cleanup', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;
// 卸载的最后一步：问要不要连配置和日志一起删。
//
// 默认保留（默认按钮是「否」）—— 用户重装时还能续上原来的设置。
// 想彻底清干净的人在这里选「是」就行。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\AirPlayBridge');
    if DirExists(DataDir) then
    begin
      // 静默卸载（/VERYSILENT，比如脚本或包管理器调用的）不能弹框 ——
      // 没人回答它，Inno 会取默认值，很容易误删用户数据。
      // 静默模式下**一律保留**配置，这也是卸载程序的通行做法。
      if UninstallSilent then exit;
      if MsgBox('要不要一并删除配置和日志？' + #13#10 + #13#10 +
                DataDir + #13#10 + #13#10 +
                '选「否」会保留它们，以后重新安装可以继续使用原有设置。',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
