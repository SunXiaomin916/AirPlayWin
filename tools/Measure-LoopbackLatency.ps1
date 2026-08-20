[CmdletBinding(DefaultParameterSetName = 'DualChannel')]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $CapturePath,

    [string] $Executable = (Join-Path $PSScriptRoot '..\build\vs2022-x64\Release\AirPlayWin.exe'),

    [Parameter(ParameterSetName = 'DualChannel')]
    [ValidateRange(0, 7)]
    [int] $ReferenceChannel = 0,

    [Parameter(Mandatory = $true, ParameterSetName = 'KnownStimulus')]
    [UInt64] $StimulusFrame,

    [ValidateRange(-1, 7)]
    [int] $OutputChannel = -1,

    [ValidateRange(0.01, 1.0)]
    [double] $OnsetThreshold = 0.25,

    [ValidateRange(1, 10000)]
    [int] $MaximumLatencyMilliseconds = 1000
)

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable -ErrorAction Stop).Path
$resolvedCapture = (Resolve-Path -LiteralPath $CapturePath -ErrorAction Stop).Path
if ($OutputChannel -lt 0) {
    $OutputChannel = if ($PSCmdlet.ParameterSetName -eq 'KnownStimulus') { 0 } else { 1 }
}

$arguments = @(
    '--analyze-loopback', $resolvedCapture,
    '--output-channel', $OutputChannel,
    '--onset-threshold', $OnsetThreshold.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--max-latency-ms', $MaximumLatencyMilliseconds
)
if ($PSCmdlet.ParameterSetName -eq 'KnownStimulus') {
    $arguments += @('--stimulus-frame', $StimulusFrame)
}
else {
    $arguments += @('--reference-channel', $ReferenceChannel)
}

& $resolvedExecutable @arguments
exit $LASTEXITCODE
