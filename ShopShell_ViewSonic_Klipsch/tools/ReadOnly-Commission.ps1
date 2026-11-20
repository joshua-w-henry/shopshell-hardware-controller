# Read-only diagnostics; does not send commands to switch sources or power.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$ControllerIp)
$base = "http://$ControllerIp"
foreach ($path in @('/health','/viewsonic/status','/viewsonic/input/status',
 '/klipsch/status','/system/startup/status')) {
    Write-Host "GET $base$path"
    try {
        $r = Invoke-RestMethod -Uri "$base$path" -Method Get -TimeoutSec 5
        $r | Format-List | Out-String | Write-Host
    } catch { Write-Warning "$path : $($_.Exception.Message)" }
}
