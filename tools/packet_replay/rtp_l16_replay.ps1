[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$Port,

    [string]$DestinationAddress = "127.0.0.1",

    [ValidateRange(1, 1000000)]
    [int]$PacketCount = 256,

    [ValidateRange(1, 8192)]
    [int]$FramesPerPacket = 352,

    [ValidateRange(8000, 384000)]
    [int]$SampleRate = 44100,

    [ValidateRange(1, 8)]
    [int]$ChannelCount = 2,

    [ValidateRange(0, 127)]
    [int]$PayloadType = 96,

    [ValidateRange(0, 65535)]
    [int]$StartSequence = 0,

    [ValidateRange(0, 4294967295)]
    [long]$StartTimestamp = 0,

    [ValidateRange(0, 1000000)]
    [int]$DropEvery = 0,

    [ValidateRange(0, 1000000)]
    [int]$DuplicateEvery = 0,

    [ValidateRange(0, 1000000)]
    [int]$RetransmitEvery = 0,

    [ValidateRange(0, 65535)]
    [int]$ControlPort = 0,

    [switch]$ReorderPairs,

    [switch]$NoPacing
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($RetransmitEvery -gt 0 -and $ControlPort -eq 0) {
    throw "-ControlPort is required when -RetransmitEvery is enabled."
}

function New-L16RtpPacket {
    param(
        [uint16]$Sequence,
        [uint32]$Timestamp,
        [uint32]$SynchronizationSource,
        [double]$Phase
    )

    $payloadBytes = $FramesPerPacket * $ChannelCount * 2
    $packet = [byte[]]::new(12 + $payloadBytes)
    $packet[0] = 0x80
    $packet[1] = [byte]$PayloadType
    $packet[2] = [byte](($Sequence -shr 8) -band 0xFF)
    $packet[3] = [byte]($Sequence -band 0xFF)
    $packet[4] = [byte](($Timestamp -shr 24) -band 0xFF)
    $packet[5] = [byte](($Timestamp -shr 16) -band 0xFF)
    $packet[6] = [byte](($Timestamp -shr 8) -band 0xFF)
    $packet[7] = [byte]($Timestamp -band 0xFF)
    $packet[8] = [byte](($SynchronizationSource -shr 24) -band 0xFF)
    $packet[9] = [byte](($SynchronizationSource -shr 16) -band 0xFF)
    $packet[10] = [byte](($SynchronizationSource -shr 8) -band 0xFF)
    $packet[11] = [byte]($SynchronizationSource -band 0xFF)

    for ($frame = 0; $frame -lt $FramesPerPacket; ++$frame) {
        $position = $Phase + $frame
        $signedSample = [int16][Math]::Round(
            [Math]::Sin(2.0 * [Math]::PI * 440.0 * $position / $SampleRate) * 8192.0)
        $sampleBits = ([int]$signedSample) -band 0xFFFF
        for ($channel = 0; $channel -lt $ChannelCount; ++$channel) {
            $offset = 12 + (($frame * $ChannelCount + $channel) * 2)
            $packet[$offset] = [byte](($sampleBits -shr 8) -band 0xFF)
            $packet[$offset + 1] = [byte]($sampleBits -band 0xFF)
        }
    }
    return $packet
}

function Add-RetransmissionWrapper {
    param([byte[]]$Packet)

    $wrapped = [byte[]]::new($Packet.Length + 4)
    $wrapped[0] = 0x80
    $wrapped[1] = 0xD6
    $wrapped[2] = 0x00
    $wrapped[3] = 0x01
    [Array]::Copy($Packet, 0, $wrapped, 4, $Packet.Length)
    return $wrapped
}

$udpClient = [System.Net.Sockets.UdpClient]::new()
$ssrc = [uint32]0x41505734
$heldPacket = $null
$heldPort = 0
$sentPackets = 0
$droppedPackets = 0
$duplicatedPackets = 0
$retransmittedPackets = 0
$packetDurationMilliseconds = 1000.0 * $FramesPerPacket / $SampleRate

try {
    for ($index = 0; $index -lt $PacketCount; ++$index) {
        $sequence = [uint16](($StartSequence + $index) -band 0xFFFF)
        $timestamp = [uint32](
            ([uint64]$StartTimestamp + ([uint64]$index * $FramesPerPacket)) -band 0xFFFFFFFFL)
        $packet = New-L16RtpPacket -Sequence $sequence -Timestamp $timestamp `
            -SynchronizationSource $ssrc -Phase ($index * $FramesPerPacket)

        if ($DropEvery -gt 0 -and (($index + 1) % $DropEvery) -eq 0) {
            ++$droppedPackets
            continue
        }

        $destinationPort = $Port
        if ($RetransmitEvery -gt 0 -and (($index + 1) % $RetransmitEvery) -eq 0) {
            $packet = Add-RetransmissionWrapper -Packet $packet
            $destinationPort = $ControlPort
            ++$retransmittedPackets
        }

        if ($ReorderPairs -and $null -eq $heldPacket) {
            $heldPacket = $packet
            $heldPort = $destinationPort
            continue
        }

        [void]$udpClient.Send($packet, $packet.Length, $DestinationAddress, $destinationPort)
        ++$sentPackets
        if ($DuplicateEvery -gt 0 -and (($index + 1) % $DuplicateEvery) -eq 0) {
            [void]$udpClient.Send($packet, $packet.Length, $DestinationAddress, $destinationPort)
            ++$sentPackets
            ++$duplicatedPackets
        }
        if ($null -ne $heldPacket) {
            [void]$udpClient.Send($heldPacket, $heldPacket.Length, $DestinationAddress, $heldPort)
            ++$sentPackets
            $heldPacket = $null
            $heldPort = 0
        }
        if (-not $NoPacing) {
            Start-Sleep -Milliseconds ([Math]::Max(1, [int][Math]::Round($packetDurationMilliseconds)))
        }
    }
    if ($null -ne $heldPacket) {
        [void]$udpClient.Send($heldPacket, $heldPacket.Length, $DestinationAddress, $heldPort)
        ++$sentPackets
    }
} finally {
    $udpClient.Dispose()
}

Write-Output "sent=$sentPackets dropped=$droppedPackets duplicated=$duplicatedPackets retransmitted=$retransmittedPackets destination=${DestinationAddress}:$Port"
