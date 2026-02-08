; xbledctl Inno Setup script
; Builds a single setup.exe that installs the app + UsbDk

[Setup]
AppName=Xbox LED Control
AppVersion=1.0.0
AppPublisher=Leclowndu93150
AppPublisherURL=https://github.com/Leclowndu93150/xbledctl
DefaultDirName={autopf}\xbledctl
DefaultGroupName=Xbox LED Control
OutputBaseFilename=xbledctl-setup
OutputDir=..\dist
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName=Xbox LED Control
MinVersion=10.0

[Files]
; Application
Source: "..\build\xbledctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\lib\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion

; UsbDk installer - user must download from https://github.com/daynix/UsbDk/releases
; and place UsbDk_1.0.22_x64.msi in this directory
Source: "UsbDk_1.0.22_x64.msi"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedUsbDk

[Icons]
Name: "{group}\Xbox LED Control"; Filename: "{app}\xbledctl.exe"
Name: "{group}\Uninstall Xbox LED Control"; Filename: "{uninstallexe}"

[Run]
; Install UsbDk silently if not already present
Filename: "msiexec.exe"; \
    Parameters: "/i ""{tmp}\UsbDk_1.0.22_x64.msi"" /qn /norestart"; \
    StatusMsg: "Installing UsbDk USB filter driver..."; \
    Flags: runhidden waituntilterminated; \
    Check: NeedUsbDk

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
function NeedUsbDk(): Boolean;
begin
  { Check if UsbDk is already installed by looking for its helper DLL }
  Result := not FileExists(ExpandConstant('{sys}\UsbDkHelper.dll'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if NeedUsbDk() then
    begin
      MsgBox(
        'The UsbDk USB filter driver was installed.' + #13#10 + #13#10 +
        'A reboot is required for the driver to become active. ' +
        'Please restart your computer before using Xbox LED Control.',
        mbInformation, MB_OK);
    end;
  end;
end;
