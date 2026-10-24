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
; ⚠️ UsePreviousAppDir 保持默认值（yes）—— 不要改成 no。
;
; 它决定「升级时用哪个目录」：yes = 沿用上次安装的位置，no = 每次都用
; 上面的 DefaultDirName。
;
; 我一度把它改成 no，理由是「防止装了新版却装到别处」，**那个判断是错的**：
; 关掉之后，先前装在非默认位置（比如用户选了 D 盘）的那份不会被碰，
; 新版装到默认目录 —— 于是机器上**同时存在两份**。
; 而放行规则和开机自启都按 exe 完整路径记录，只能指向一份：
; 结果就是新装的那份没被放行 → 连不上 + 每次启动都提示未被防火墙放行。
;
; 当时看到的「装到别处」其实是我自己用 /DIR= 指向临时目录做验证造成的，
; 后续安装跟着跑到那儿 —— 那是**测试污染**，不是这个选项的缺陷。
; 真实用户不会这么干。
;
; 如果将来真要处理「用户换过目录」，正确做法是在安装前检测并提示，
; 而不是强行忽略他原来的选择。
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=AirPlay-Bridge-Setup-v{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 安装包自己的图标（右键 Setup.exe → 属性 / 资源管理器里显示的那一枚）。
; 不设的话是 Inno 的默认图标，跟程序图标不是一个东西，用户会以为是别的东西。
; 路径相对于 .iss 所在目录（installer\），所以是 ..\src\icons\app.ico。
; 用的是与 exe 同一枚图标，保持一致。
; （2026-10-24 遗留清单第 2 条）
SetupIconFile=..\src\icons\app.ico
; 安装包自身的版本信息（右键 Setup.exe → 属性 → 详细信息）。
;
; 不设这几项的话，属性页里 FileVersion 是**空的** ——
; 用户报"我装的是哪个版本"时，点开安装包属性看不到任何东西，
; 只能靠文件名。而文件名是可以被改的。
; （2026-10-24 实测 v1.5.42 的包：ProductName 有、FileVersion 空）
;
; VersionInfoVersion 要的是四段数字（x.y.z.w），而 AppVersion 是
; "1.5.42" 这种三段字符串 —— 拼一个 .0 补满。
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoCopyright=Apache-2.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#ExeName}
; 装到 Program Files 需要管理员提权。
; 注意：程序**运行**时不需要管理员 —— 配置和日志都在 %LOCALAPPDATA%，
; 只有加防火墙规则那一步会自己单独提权。
PrivilegesRequired=admin
; ⚠️ 这里**故意不设 AppMutex**。
;
; AppMutex 表面上是「让 Inno 知道该关哪个程序」，实际会**挡住安装**：
; Inno 在安装流程的**最开头**就检查它，而那时我们自己的「结束旧实例」
; 还没机会跑（那在 PrepareToInstall，属于后面一步）。于是只要程序正在运行，
; 安装就在这一步被中止 —— 静默安装下更是**一声不响地什么都没做**，
; 用户看到的就是「装了新版，打开还是旧版」。实测踩到。
;
; 结束旧实例我们自己在 [Code] 里做（见 KillOldInstance），不需要 Inno 帮忙；
; 而本程序是托盘程序、没有主窗口，Inno 的自动关闭机制本来也关不掉它。
CloseApplications=no
RestartApplications=no

; 语言选择按系统「UI 语言」匹配（Inno 的默认行为，这里写出来是为了不被
; 将来谁改成 none 而悄悄失效）：中文系统自动选 chinese，其余回落 english。
; App 界面本身只有中英两种，别的语言没有对应译文。
;
; ★ 指令名是 LanguageDetectionMethod —— 曾误写成 LanguagesDetection，
;   Inno 报「未知指令」直接中止打包，整个 release 卡在打包这一步。
;   合法值只有 uilanguage / locale / none。
LanguageDetectionMethod=uilanguage

[Languages]
; 语言文件随仓库走 —— CI 上新装的 Inno 不自带简体中文
;
; ★ 顺序 = 系统语言匹配不上时的回退，所以 english 必须排第一。
;   原来 chinese 排第一，德语/日语等任何非中英系统都会默认预选简体中文，
;   用户第一眼看到的是看不懂的界面。
;   正常情况由 [Setup] 的 LanguageDetectionMethod=uilanguage 决定：
;   中文系统选 chinese，其余一律回落到这里的英文。
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinese"; MessagesFile: "ChineseSimplified.isl"

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
; ★ 这里**故意是空的**，不要加 `Type: filesandordirs; Name: "{app}"`。
;
; 那条规则会**递归删除整个安装目录**，包括用户自己放进去的东西：
; 安装时目录可以随便改（DefaultDirName 只是默认值），一旦有人装到
; D:\Tools 这类已有内容的目录，卸载就等于删库 —— 不可逆的数据丢失。
; （2026-10-18 第四轮外部审查 P1-1）
;
; 不需要它：[Files] 里那 4 个文件（exe + README + CHANGELOG + LICENSE）
; Inno 自己会记录并在卸载时删掉；目录空了它会自己清掉。
; 也就是说这条规则**唯一的作用就是删掉原本不该删的东西**。
;
; 将来若要清理运行期产物（日志、配置），请**逐项列明具体文件**，
; 绝不要对整个 {app} 递归。

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
  Tool: String;
begin
  // 必须用完整路径。Inno 的 Exec 靠 PATH 找可执行文件，实测找不到
  // taskkill —— 结果就是「旧实例没被杀掉」，而它占着 exe 文件，
  // 安装跳过不覆盖，装完还是旧版本（表现为「装了新版还是老样子」）。
  Tool := ExpandConstant('{sys}\\taskkill.exe');
  if not Exec(Tool, '/F /IM "' + AN + '"', '', SW_HIDE, ewWaitUntilTerminated, R) then
    Log('结束旧实例：无法启动 taskkill（' + Tool + '）')
  else
    Log('结束旧实例：' + AN + ' -> taskkill 返回 ' + IntToStr(R));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  KillOldInstance('AirPlayBridge.exe');
  KillOldInstance('AirPlay Bridge.exe');
  Sleep(2000);
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
  Cleaner: String;
begin
  Result := True;
  Cleaner := ExpandConstant('{app}\{#ExeName}');
  if FileExists(Cleaner) then
  begin
    // ★ 检查 Exec 的返回值**和**退出码，失败要告诉用户。
    //   原来直接忽略 —— 清理跑不起来（被杀软拦住、权限不足、exe 已损坏）
    //   时，防火墙规则和开机自启项会留在系统里变成指向不存在文件的死记录，
    //   用户重装后还会看到，而且不知道去哪清（2026-10-18 审查 P3-3）。
    //
    //   不把 Result 置 False：清理失败不该**阻止**卸载 —— 那样用户删不掉程序。
    //   只提示，让他知道手动去哪清。
    if (not Exec(Cleaner, '--uninstall-cleanup', '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode))
       or (ResultCode <> 0) then
    begin
      // ★ 静默卸载（/SILENT、/VERYSILENT）下绝不能弹 MsgBox ——
      //   没人会点「确定」，安装程序会**永久挂起**，批量部署时卡死在那台机器上。
      //   静默场景改为写一条卸载日志，用户可以事后查看。
      if UninstallSilent then
        Log('系统集成清理未完成：防火墙规则 / 开机自启项可能残留。')
      else
        MsgBox('系统集成清理未完成（防火墙规则 / 开机自启项可能残留）。' + #13#10 + #13#10 +
               '不影响卸载，但建议在卸载后手动检查：' + #13#10 +
               '  · Windows 防火墙 → 高级设置 → 入站规则' + #13#10 +
               '  · 任务管理器 → 启动应用' + #13#10 + #13#10 +
               '程序仍会被正常卸载。',
               mbInformation, MB_OK);
    end;
  end;
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
