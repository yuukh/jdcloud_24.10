param(
    [string]$Router = '',
    [string]$Iperf = (Join-Path $PSScriptRoot 'iperf3.exe')
)
$ErrorActionPreference = 'Stop'
if (-not $Router) {
    $Router = Read-Host 'Router IPv4 address [192.168.3.1]'
    if (-not $Router) { $Router = '192.168.3.1' }
}
$parsed = $null
if (-not [System.Net.IPAddress]::TryParse($Router, [ref]$parsed) -or
    $parsed.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork -or
    $Router -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
    throw 'Use a literal router IPv4 address.'
}
if (-not (Test-Path -LiteralPath $Iperf -PathType Leaf)) {
    $found = Get-Command iperf3.exe -ErrorAction SilentlyContinue
    if (-not $found) { throw 'Place your existing iperf3.exe and its DLLs beside this script.' }
    $Iperf = $found.Source
}
$Iperf = (Resolve-Path -LiteralPath $Iperf).Path
$ssh = (Get-Command ssh.exe -ErrorAction Stop).Source
$scp = (Get-Command scp.exe -ErrorAction Stop).Source
$output = Join-Path $PSScriptRoot ('results-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output | Out-Null
& $Iperf --version | Set-Content -Encoding UTF8 (Join-Path $output 'client-version.txt')
Write-Host 'Keep this computer on Wi-Fi. Stop other iperf servers/tests.'
Write-Host 'Confirm the router SSH host fingerprint, then enter the SSH password.'
Write-Host 'Four single-stream reverse tests run automatically; do not close SSH.'

$job = Start-Job -ArgumentList $Router,$Iperf,$output -ScriptBlock {
    param($router,$iperf,$output)
    $ErrorActionPreference = 'Continue'
    foreach ($port in @(5201,5202,5203,5204)) {
        $deadline = (Get-Date).AddMinutes(5)
        $done = $false
        $attempt = 0
        while ((Get-Date) -lt $deadline) {
            $attempt++
            # Never probe an iperf port with a raw TCP connection: that would
            # consume the server's one-off control session.
            $text = & $iperf -4 -c $router -p $port -R -P 1 -t 60 -i 1 -J --connect-timeout 1000 2>&1 | Out-String
            $code = $LASTEXITCODE
            if ($code -eq 0) {
                $text | Set-Content -Encoding UTF8 (Join-Path $output "client-$port.json")
                $done = $true
                break
            }
            # Retain only the latest failure, not an unbounded retry log.
            $text | Set-Content -Encoding UTF8 (Join-Path $output "last-error-$port.txt")
            Start-Sleep -Seconds 2
        }
        if (-not $done) { throw "iperf port $port did not complete; see last-error-$port.txt" }
    }
    'All four client tests completed.'
}

try {
    & $ssh -4 -o StrictHostKeyChecking=ask -o ServerAliveInterval=10 -o ServerAliveCountMax=3 "root@$Router" /usr/sbin/hnat418-run |
        Tee-Object -FilePath (Join-Path $output 'router-control.txt')
    $remoteExit = $LASTEXITCODE
    if ($remoteExit -ne 0) {
        Stop-Job $job -ErrorAction SilentlyContinue
        if (Select-String -Path (Join-Path $output 'router-control.txt') -Pattern '^HNAT418 ARCHIVE ' -Quiet) {
            & $scp -4 -o StrictHostKeyChecking=yes "root@${Router}:/overlay/hnat418-debug/latest.tar.gz" (Join-Path $output 'router-partial-evidence.tar.gz')
        }
        throw 'Router stopped the test. Partial evidence was retained; check router-control.txt.'
    }
    Wait-Job $job -Timeout 15 | Out-Null
    Receive-Job $job
    if ($job.State -ne 'Completed') { throw 'Client test job did not complete successfully.' }
    Write-Host 'Downloading the router evidence archive (SSH may request the password again).'
    & $scp -4 -o StrictHostKeyChecking=yes "root@${Router}:/overlay/hnat418-debug/latest.tar.gz" (Join-Path $output 'router-evidence.tar.gz')
    if ($LASTEXITCODE -ne 0) { throw 'Download failed; the archive remains on the router.' }
    Write-Host "Evidence saved in $output"
} finally {
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
}
