' Launches avatar.exe the way a double-click in Explorer does: from a host with no
' console, so the app cannot attach to a parent terminal and takes its windowed
' start path, where stdout and stderr are reopened onto the log file.
'
' Every shell launch -- cmd, PowerShell, Git Bash, an agent's Bash tool -- has a
' console the app attaches to instead, so none of them exercise that path. On
' 22 Sep 2026 it had been broken for a day without any run noticing. This is
' the acceptance step for it: run it, wait, and read the log.
'
'   wscript scripts\launch_no_console.vbs [seconds]
'
' Waits for the app to exit, then prints nothing; the log is
' %APPDATA%\AIInterface\logs\avatar.log and should end with "avatar exit".
Dim shell, fso, here, exe, secs
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(fso.GetParentFolderName(WScript.ScriptFullName))
exe = fso.BuildPath(here, "build\bin\Release\avatar.exe")
secs = "20"
If WScript.Arguments.Count > 0 Then secs = WScript.Arguments(0)
If Not fso.FileExists(exe) Then
  WScript.Echo "not built: " & exe
  WScript.Quit 1
End If
shell.Run """" & exe & """ --seconds " & secs, 0, True
