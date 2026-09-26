[CmdletBinding()]
param(
    [string]$DestinationAddress = "127.0.0.1",

    [ValidateRange(1, 65535)]
    [int]$EventPort = 319,

    [ValidateRange(1, 65535)]
    [int]$GeneralPort = 320,

    [ValidateRange(1, 100000)]
    [int]$Count = 20,

    [ValidateRange(10, 60000)]
    [int]$IntervalMilliseconds = 1000,

    [ValidateRange(-1000.0, 1000.0)]
    [double]$DriftPpm = 0.0,

    [ValidateRange(0, 1000000)]
    [int]$JumpMicroseconds = 0,

    [ValidateRange(0, 100000)]
    [int]$JumpAt = 0,

    [ValidateRange(0, 255)]
    [int]$Domain = 0,

    [switch]$TwoStep,

    [switch]$NoPacing
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($JumpAt -gt $Count) {
    throw "-JumpAt cannot exceed -Count."
}

function Set-U16Be {
    param([byte[]]$Buffer, [int]$Offset, [uint16]$Value)
    $Buffer[$Offset] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 1] = [byte]($Value -band 0xFF)
}

function Set-U32Be {
    param([byte[]]$Buffer, [int]$Offset, [uint32]$Value)
    $Buffer[$Offset] = [byte](($Value -shr 24) -band 0xFF)
    $Buffer[$Offset + 1] = [byte](($Value -shr 16) -band 0xFF)
    $Buffer[$Offset + 2] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 3] = [byte]($Value -band 0xFF)
}

function Set-U64Be {
    param([byte[]]$Buffer, [int]$Offset, [uint64]$Value)
    for ($index = 0; $index -lt 8; ++$index) {
        $Buffer[$Offset + $index] =
            [byte](($Value -shr ((7 - $index) * 8)) -band 0xFF)
    }
}

function New-PtpPacket {
    param(
        [ValidateSet(0, 8)]
        [int]$MessageType,
        [uint16]$Sequence,
        [uint64]$TimestampNanoseconds,
        [bool]$UseTwoStep
    )

    $packet = [byte[]]::new(44)
    $packet[0] = [byte]$MessageType
    $packet[1] = 2
    Set-U16Be -Buffer $packet -Offset 2 -Value ([uint16]$packet.Length)
    $packet[4] = [byte]$Domain
    if ($UseTwoStep -and $MessageType -eq 0) {
        Set-U16Be -Buffer $packet -Offset 6 -Value 0x0200
    }
    Set-U64Be -Buffer $packet -Offset 20 -Value ([uint64]0x4150575054503031)
    Set-U16Be -Buffer $packet -Offset 28 -Value 1
    Set-U16Be -Buffer $packet -Offset 30 -Value $Sequence

    $seconds = [uint64][Math]::Floor($TimestampNanoseconds / 1000000000.0)
    $nanoseconds = [uint32]($TimestampNanoseconds % [uint64]1000000000)
    for ($index = 0; $index -lt 6; ++$index) {
        $packet[34 + $index] =
            [byte](($seconds -shr ((5 - $index) * 8)) -band 0xFF)
    }
    Set-U32Be -Buffer $packet -Offset 40 -Value $nanoseconds
    return $packet
}

$eventClient = [System.Net.Sockets.UdpClient]::new()
$generalClient = [System.Net.Sockets.UdpClient]::new()
$baseNanoseconds = [uint64]1700000000000000000
$remoteScale = 1.0 + ($DriftPpm / 1000000.0)
$sentEvent = 0
$sentGeneral = 0
$eventClient.Connect($DestinationAddress, $EventPort)
$generalClient.Connect($DestinationAddress, $GeneralPort)
[void](New-PtpPacket -MessageType 0 -Sequence 0 -TimestampNanoseconds $baseNanoseconds `
    -UseTwoStep ([bool]$TwoStep))
$clock = [System.Diagnostics.Stopwatch]::StartNew()

try {
    for ($index = 0; $index -lt $Count; ++$index) {
        # Base the synthetic remote clock on elapsed monotonic time so scheduler delay changes
        # packet spacing rather than masquerading as oscillator drift.
        $elapsedNanoseconds = [double]$clock.ElapsedTicks * 1000000000.0 /
            [System.Diagnostics.Stopwatch]::Frequency * $remoteScale
        if ($JumpAt -gt 0 -and ($index + 1) -ge $JumpAt) {
            $elapsedNanoseconds += [double]$JumpMicroseconds * 1000.0
        }
        $timestamp = [uint64]($baseNanoseconds + [uint64][Math]::Round($elapsedNanoseconds))
        $sequence = [uint16]($index -band 0xFFFF)
        $syncTimestamp = if ($TwoStep) { [uint64]0 } else { $timestamp }
        $sync = New-PtpPacket -MessageType 0 -Sequence $sequence `
            -TimestampNanoseconds $syncTimestamp -UseTwoStep ([bool]$TwoStep)
        [void]$eventClient.Send($sync, $sync.Length)
        ++$sentEvent

        if ($TwoStep) {
            $followUp = New-PtpPacket -MessageType 8 -Sequence $sequence `
                -TimestampNanoseconds $timestamp -UseTwoStep $false
            [void]$generalClient.Send($followUp, $followUp.Length)
            ++$sentGeneral
        }

        if (-not $NoPacing -and $index + 1 -lt $Count) {
            Start-Sleep -Milliseconds $IntervalMilliseconds
        }
    }
} finally {
    $eventClient.Dispose()
    $generalClient.Dispose()
}

Write-Host "PTP replay complete: event=$sentEvent general=$sentGeneral " `
    "twoStep=$([bool]$TwoStep) driftPpm=$DriftPpm jumpUs=$JumpMicroseconds."
Write-Host "This is a packet-path probe; PowerShell scheduling is not a clock-calibration source."
