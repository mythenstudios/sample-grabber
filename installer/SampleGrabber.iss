; Sample Grabber - https://github.com/mythenstudios/sample-grabber
; Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
; SPDX-License-Identifier: AGPL-3.0-or-later

; Sample Grabber - Windows installer (Inno Setup 6.7+, dark wizard needs 6.6+)
;
; Build (CI does this after packaging):
;   ISCC.exe /DAppVersion=0.1.0 /DPkgDir=<folder with "Sample Grabber.exe", "Sample Grabber.vst3", LICENSE> /O<outdir> installer\SampleGrabber.iss
;
; Two modes on the first page:
;   Recommended - standard folders, VST3 + Standalone + Start menu, no further questions
;   Advanced    - components, app folder, VST3/export/cache folders, desktop icon
; Folders the app reads at runtime go to HKCU\Software\Mythen Studios\Sample Grabber (ExportDir, CacheDir).
; Environment variables SG_EXPORT_DIR / SG_CACHE_DIR still win inside the app.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef PkgDir
  #define PkgDir "..\dist\pkg"
#endif

#define AppName      "Sample Grabber"
#define AppPublisher "Mythen Studios"
#define AppUrl       "https://github.com/mythenstudios/sample-grabber"
#define RegKey       "Software\Mythen Studios\Sample Grabber"

[Setup]
AppId={{5B1F3C2E-8E0A-4C55-9A57-3D0B7E1A6C42}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppCopyright=Copyright (C) Mythen Studios. AGPL-3.0-or-later.
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableReadyPage=no
LicenseFile={#PkgDir}\LICENSE
OutputBaseFilename=SampleGrabber-{#AppVersion}-windows-x64-setup
SetupIconFile=images\setup.ico
UninstallDisplayIcon={app}\{#AppName}.exe
UninstallDisplayName={#AppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UsedUserAreasWarning=no
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
; Brand: dark wizard on the app's ink colour (#0B0B0C, Inno colours are $BBGGRR)
WizardStyle=modern dark
WizardBackColor=$0C0B0B
WizardImageFile=images\wizard-100.png,images\wizard-125.png,images\wizard-150.png,images\wizard-175.png,images\wizard-200.png,images\wizard-250.png
WizardSmallImageFile=images\small-100.png,images\small-125.png,images\small-150.png,images\small-175.png,images\small-200.png,images\small-250.png

[Languages]
; English only, like the app
Name: "en"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Plugin and app"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (FL Studio, Ableton Live, Bitwig, Reaper, ...)"; Types: full custom
Name: "app";  Description: "Standalone app";                                             Types: full custom

[Tasks]
Name: "startmenu";   Description: "Start menu entry";  Components: app
Name: "desktopicon"; Description: "Desktop shortcut";  Components: app; Flags: unchecked

[Files]
Source: "{#PkgDir}\{#AppName}.exe";    DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "{#PkgDir}\LICENSE";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#PkgDir}\{#AppName}.vst3\*"; DestDir: "{code:GetVst3Dir}\{#AppName}.vst3"; Components: vst3; \
    Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\{#AppName}.exe"; Tasks: startmenu
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}";       Tasks: startmenu
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\{#AppName}.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "{#RegKey}"; ValueType: string; ValueName: "ExportDir"; ValueData: "{code:GetExportDir}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "{#RegKey}"; ValueType: string; ValueName: "CacheDir";  ValueData: "{code:GetCacheDir}";  Flags: uninsdeletekey
Root: HKCU; Subkey: "{#RegKey}"; ValueType: string; ValueName: "Vst3Dir";   ValueData: "{code:GetVst3Dir}";   Flags: uninsdeletekey; Components: vst3

[UninstallDelete]
Type: files; Name: "{app}\INSTALLED-FILES.txt"

[Run]
Filename: "{app}\{#AppName}.exe"; Description: "Start {#AppName}"; Components: app; Flags: nowait postinstall skipifsilent unchecked

[Code]
var
  ModePage: TInputOptionWizardPage;
  FolderPage: TInputDirWizardPage;

function IsAdvanced(): Boolean;
begin
  Result := (ModePage <> nil) and (ModePage.SelectedValueIndex = 1);
end;

function DefaultVst3Dir(): String;
begin
  if IsAdminInstallMode() then
    Result := ExpandConstant('{commoncf64}\VST3')
  else
    Result := ExpandConstant('{localappdata}\Programs\Common\VST3');
end;

function MusicDir(): String;
var
  S: String;
begin
  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders', 'My Music', S) and (S <> '') then
    Result := S
  else
    Result := ExpandConstant('{%USERPROFILE}\Music');
end;

function DefaultExportDir(): String;
begin
  Result := MusicDir() + '\{#AppName}';
end;

function DefaultCacheDir(): String;
begin
  Result := ExpandConstant('{localappdata}\SampleGrabber\cache');
end;

function ToolsDir(): String;
begin
  Result := ExpandConstant('{localappdata}\SampleGrabber\bin');
end;

// Values from an earlier install win over the defaults (re-install keeps the user's folders)
function PreviousOr(const Name, Fallback: String): String;
var
  S: String;
begin
  if RegQueryStringValue(HKCU, '{#RegKey}', Name, S) and (Trim(S) <> '') then
    Result := S
  else
    Result := Fallback;
end;

function GetVst3Dir(Param: String): String;
begin
  if IsAdvanced() then Result := RemoveBackslashUnlessRoot(Trim(FolderPage.Values[0])) else Result := PreviousOr('Vst3Dir', DefaultVst3Dir());
end;

function GetExportDir(Param: String): String;
begin
  if IsAdvanced() then Result := RemoveBackslashUnlessRoot(Trim(FolderPage.Values[1])) else Result := PreviousOr('ExportDir', DefaultExportDir());
end;

function GetCacheDir(Param: String): String;
begin
  if IsAdvanced() then Result := RemoveBackslashUnlessRoot(Trim(FolderPage.Values[2])) else Result := PreviousOr('CacheDir', DefaultCacheDir());
end;

procedure InitializeWizard();
begin
  ModePage := CreateInputOptionPage(wpLicense,
    'Installation', 'How do you want to install Sample Grabber?',
    'Recommended installs the VST3 plugin and the app into the standard folders. ' +
    'Advanced lets you pick components and every folder.',
    True, False);
  ModePage.Add('Recommended (standard folders)');
  ModePage.Add('Advanced (choose components and folders)');
  ModePage.SelectedValueIndex := 0;

  FolderPage := CreateInputDirPage(wpSelectComponents,
    'Folders', 'Where should Sample Grabber put its files?',
    'VST3: your DAW scans this folder for plugins.' + #13#10 +
    'Exports: the WAV files you drag into your DAW (keep them, projects point to them).' + #13#10 +
    'Cache: downloaded sources, can be cleared any time in the app (STORAGE). A folder that already holds ' +
    'other files gets its own subfolder "Sample Grabber Cache".',
    False, '');
  FolderPage.Add('VST3 plugin folder');
  FolderPage.Add('Export folder');
  FolderPage.Add('Cache folder (changes apply after the next app start)');
  FolderPage.Values[0] := PreviousOr('Vst3Dir', DefaultVst3Dir());
  FolderPage.Values[1] := PreviousOr('ExportDir', DefaultExportDir());
  FolderPage.Values[2] := PreviousOr('CacheDir', DefaultCacheDir());
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if not IsAdvanced() then
    Result := (PageID = wpSelectDir) or (PageID = wpSelectComponents) or (PageID = wpSelectTasks)
              or (PageID = FolderPage.ID);
  // Folder page: the VST3 line only matters with the plugin selected, but the page is still needed for exports/cache
end;

function IsInside(const Child, Parent: String): Boolean;
var
  C, P: String;
begin
  C := AddBackslash(Lowercase(Child));
  P := AddBackslash(Lowercase(Parent));
  Result := Copy(C, 1, Length(P)) = P;
end;

function CheckFolder(const Name, Dir: String): Boolean;
begin
  Result := False;
  if Dir = '' then
    MsgBox(Name + ': please enter a folder.', mbError, MB_OK)
  else if (Length(Dir) < 3) or (Copy(Dir, 2, 2) <> ':\') and (Copy(Dir, 1, 2) <> '\\') then
    MsgBox(Name + ': please enter a full path, e.g. D:\Samples.', mbError, MB_OK)
  else if RemoveBackslashUnlessRoot(Dir) = ExtractFileDrive(Dir) + '\' then
    MsgBox(Name + ': a drive root is not allowed, please pick a folder.', mbError, MB_OK)
  else
    Result := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  E, C: String;
begin
  Result := True;
  if CurPageID = FolderPage.ID then
  begin
    E := GetExportDir('');
    C := GetCacheDir('');
    Result := CheckFolder('Export folder', E) and CheckFolder('Cache folder', C);
    if Result and WizardIsComponentSelected('vst3') then
      Result := CheckFolder('VST3 folder', GetVst3Dir(''));
    if Result and (IsInside(E, C) or IsInside(C, E)) then
    begin
      MsgBox('Export and cache folder must be separate (the cache can be cleared, exports must stay).', mbError, MB_OK);
      Result := False;
    end;
  end;
  if (CurPageID = wpSelectComponents) and not WizardIsComponentSelected('vst3') and not WizardIsComponentSelected('app') then
  begin
    MsgBox('Select at least the plugin or the app.', mbError, MB_OK);
    Result := False;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  S := 'WHAT GOES WHERE' + NewLine;
  if WizardIsComponentSelected('vst3') then
    S := S + Space + 'VST3 plugin:  ' + GetVst3Dir('') + '\{#AppName}.vst3' + NewLine;
  S := S + Space + 'App folder:   ' + ExpandConstant('{app}') + NewLine;
  S := S + Space + 'Exports:      ' + GetExportDir('') + NewLine;
  S := S + Space + 'Cache:        ' + GetCacheDir('') + NewLine;
  if WizardIsTaskSelected('startmenu') then
    S := S + Space + 'Start menu:   ' + ExpandConstant('{group}') + NewLine;
  if WizardIsTaskSelected('desktopicon') then
    S := S + Space + 'Desktop:      shortcut' + NewLine;
  if not IsAdminInstallMode() and WizardIsComponentSelected('vst3') then
    S := S + NewLine + Space + 'Note: installed for you only. If your DAW does not find the plugin, add the VST3 folder ' +
         'above to its plugin folders.' + NewLine;

  S := S + NewLine + 'DOWNLOADED BY THE APP (only when you click INSTALL TOOLS)' + NewLine;
  S := S + Space + 'yt-dlp, deno, ffmpeg (~385 MB) into ' + ToolsDir() + NewLine;
  S := S + Space + 'Source: the official GitHub releases, checked with SHA-256.' + NewLine;

  S := S + NewLine + 'THE UNINSTALLER REMOVES' + NewLine;
  S := S + Space + 'Plugin, app, shortcuts, downloaded tools, cache, app settings.' + NewLine;
  S := S + Space + 'Your exports stay unless you choose to delete them.' + NewLine;
  S := S + Space + 'Plugin lists inside your DAW are not touched.' + NewLine;
  Result := S;
end;

procedure WriteInstalledFiles();
var
  L: TStringList;
  A: TArrayOfString;
  I: Integer;
begin
  L := TStringList.Create;
  try
    L.Add('Sample Grabber {#AppVersion} - installed files and folders');
    L.Add('');
    if WizardIsComponentSelected('app') then
      L.Add('App:         ' + ExpandConstant('{app}\{#AppName}.exe'));
    if WizardIsComponentSelected('vst3') then
      L.Add('VST3:        ' + GetVst3Dir('') + '\{#AppName}.vst3');
    L.Add('Exports:     ' + GetExportDir(''));
    L.Add('Cache:       ' + GetCacheDir(''));
    L.Add('Tools:       ' + ToolsDir() + '  (yt-dlp, deno, ffmpeg - downloaded by the app)');
    L.Add('Settings:    ' + ExpandConstant('{userappdata}\SampleGrabber'));
    L.Add('Registry:    HKCU\{#RegKey}');
    L.Add('');
    L.Add('The uninstaller removes everything above except the exports (asked separately).');
    // UTF-8: user names with umlauts appear in the paths
    SetArrayLength(A, L.Count);
    for I := 0 to L.Count - 1 do
      A[I] := L[I];
    SaveStringsToUTF8File(ExpandConstant('{app}\INSTALLED-FILES.txt'), A, False);
  finally
    L.Free;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    WriteInstalledFiles();
end;

// ---------------------------------------------------------------- uninstall

var
  UninstExportDir, UninstCacheDir: String;

// Only our own subfolders and the marker: a cache folder the user picked may hold other files
procedure RemoveCache(const Dir: String);
var
  D: String;
begin
  D := Lowercase(RemoveBackslashUnlessRoot(Dir));
  if (Dir = '') or not DirExists(Dir) then
    Exit;
  // Never inside a drive root, the profile or the Music folder, even with a marker
  if (D = Lowercase(ExtractFileDrive(Dir) + '\')) or (D = Lowercase(ExpandConstant('{%USERPROFILE}')))
     or (D = Lowercase(MusicDir())) then
    Exit;
  if not FileExists(Dir + '\.sg-cache') then
    Exit;
  DelTree(Dir + '\downloads', True, True, True);
  DelTree(Dir + '\imports', True, True, True);
  DelTree(Dir + '\staging', True, True, True);
  DelTree(Dir + '\tmp', True, True, True);
  DeleteFile(Dir + '\.sg-cache');
  RemoveDir(Dir);
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if not RegQueryStringValue(HKCU, '{#RegKey}', 'ExportDir', UninstExportDir) then
    UninstExportDir := DefaultExportDir();
  if not RegQueryStringValue(HKCU, '{#RegKey}', 'CacheDir', UninstCacheDir) then
    UninstCacheDir := DefaultCacheDir();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Base: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;

  RemoveCache(UninstCacheDir);
  RemoveCache(UninstCacheDir + '\Sample Grabber Cache');   // the app's own subfolder inside a non-empty chosen folder
  RemoveCache(DefaultCacheDir());
  Base := ExpandConstant('{localappdata}\SampleGrabber');
  DelTree(Base + '\bin', True, True, True);
  RemoveDir(Base);
  DelTree(ExpandConstant('{userappdata}\SampleGrabber'), True, True, True);

  // Exports: only on request (default No). Only a folder named "Sample Grabber" is removed as a whole,
  // anything else may be a folder the user shares with other files.
  if DirExists(UninstExportDir) and not UninstallSilent() then
  begin
    if CompareText(ExtractFileName(RemoveBackslashUnlessRoot(UninstExportDir)), '{#AppName}') = 0 then
    begin
      if MsgBox('Also delete your exported samples?' + #13#10 + #13#10 + UninstExportDir + #13#10 + #13#10 +
                'DAW projects that use them will miss these files.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(UninstExportDir, True, True, True);
    end
    else
      MsgBox('Your exported samples were kept:' + #13#10 + UninstExportDir, mbInformation, MB_OK);
  end;
end;
