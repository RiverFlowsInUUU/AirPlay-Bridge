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
; 永远装上 DefaultDirName，不沿用上次装的目录。
;
; 这个选项默认是 yes —— Inno 会读注册表里上次的安装路径。
; 对本程序来说那是**有害**的：安装版的全部价值就是「固定路径」，
; 防火墙放行和开机自启都按这个路径记录。一旦它跟着上次的目录跑，
; 用户就会遇到「明明装了新版，还是老样子」——因为新版装到了别处，
; 原来那个位置的文件压根没被碰。
;（实测踩到：把包解开到临时目录验证一次，后续安装全都跟着跑到那儿去了。）
UsePreviousAppDir=no
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
; 单实例互斥体名 —— 必须和程序里 kMutexName 完全一致。
; Inno 靠它识别「是哪个程序占着文件」。
AppMutex=Global\AirPlayBridge_SingleInstance
; 不劳烦 Inno 的自动关闭：本程序是托盘程序、**没有主窗口**，
; 它靠窗口找进程，找不到 → 只弹一个「请关闭 X」的提示；
; 用户点了「确定」文件依然被占用，安装就卡在那儿过不去。
; 改为在 PrepareToInstall 里主动结束（见 [Code]）。
CloseApplications=no
RestartApplications=no

[Languages]
; 语言文件随仓库走 —— CI 上新装的 Inno 不自带简体中文
Name: "chinese"; MessagesFile: "ChineseSimplified.isl"
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
; 安装时就把网络放行规则加好。
;
; 接收端要主动 UDP 回连本机做时钟同步，没有这条规则会一直卡在「连接中」。
; 安装程序本来就是管理员权限，顺手做掉比让用户装完再去菜单里点一次舒服。
;
; 用 --quiet 让它别弹提示框 —— 用户在等安装完成，不该再冒一个框；
; 成功与否都写进日志，而且程序启动时的自检还有一道兜底提示。
Filename: "{app}\{#ExeName}"; Parameters: "--fix-firewall --quiet"; StatusMsg: "正在配置网络放行…"; Flags: runhidden waituntilterminated
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
// 开始复制文件之前，主动结束还在跑的旧实例。
//
// 为什么必须主动做：本程序是托盘程序，**没有主窗口** —— Inno 的
// CloseApplications / Restart Manager 依赖窗口来定位进程，找不到它，
// 于是只弹一句「请关闭 AirPlay Bridge」。用户点了「确定」，文件依然被占用，
// 安装就卡在那里过不去 —— 结果是装到一半停住、放行规则没配上、
// 装完连不上，还每次启动都提示防火墙（用户实际踩到）。
//
// 两个进程名都要处理：安装版叫 AirPlayBridge.exe，
// 而旧的免安装版叫 AirPlay Bridge.exe。两者共用同一个全局互斥体，
// 任意一个在跑都会挡住安装。
procedure KillOldInstance(const AN: String);
var
  R: Integer;
begin
  Exec('taskkill.exe', '/F /IM "' + AN + '"', '', SW_HIDE, ewWaitUntilTerminated, R);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  KillOldInstance('AirPlayBridge.exe');
  KillOldInstance('AirPlay Bridge.exe');
  Sleep(1000);
end;

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
