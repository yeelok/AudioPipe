<#
.SYNOPSIS
    Pin the AudioPipe (Virtual-Audio-Driver) render endpoint to 48 kHz only.

.DESCRIPTION
    AudioPipe is a fixed 48 kHz / float32 pipeline: the wire protocol carries no
    sample rate, and the macOS receiver plays into a 48 kHz device with only a
    +/-0.1% drift trim (NOT a sample-rate converter). If the Windows endpoint's
    shared-mode mix format is anything other than 48 kHz, the sender captures at
    that rate and the receiver's playout ring starves (lower rates) or overflows
    (higher rates) -> dropouts, clicks and pitch shift.

    Upstream's speaker endpoint advertises 8 kHz - 192 kHz (incl. 44.1 kHz), so
    Windows is free to pick a non-48k mix format. This script edits
    Source/Filters/speakerwavtable.h in the upstream clone so the speaker
    endpoint advertises ONLY 48 kHz (16/24/32-bit). Windows then has exactly one
    rate to choose and the mix format is always 48 kHz -> fully bit-transparent,
    no resampling anywhere in AudioPipe.

    It rewrites three things that must agree:
      1. SpeakerHostPinSupportedDeviceFormats[]  -> three 48 kHz entries.
      2. SpeakerHostPinSupportedDeviceModes[]     -> point only at those.
      3. SpeakerPinDataRangesStream[]             -> Min=Max=48000 Hz.
    (and the documentation-only SPEAKER_HOST_MAX_SAMPLE_RATE macro).

    Idempotent; safe to re-run. Run AFTER apply_rename.ps1 and BEFORE build.ps1.
    The regex patterns + replacement bodies were validated against upstream
    commit's speakerwavtable.h on a non-Windows box with the equivalent .NET
    regex semantics (Singleline, non-greedy). If a future upstream refactor
    changes the table shape, this script throws instead of silently leaving
    non-48k formats in place.

.EXAMPLE
    .\driver-win\apply_48k.ps1
#>
param(
    [string]$Upstream = "$PSScriptRoot\upstream"
)
$ErrorActionPreference = 'Stop'

$h = Join-Path $Upstream "Source\Filters\speakerwavtable.h"
if (-not (Test-Path $h)) {
    throw "speaker wave table not found: $h`nClone upstream first (see driver-win\README.md)."
}

$marker = 'pinned to 48 kHz for AudioPipe'
$text = [System.IO.File]::ReadAllText($h)
if ($text.Contains($marker)) {
    Write-Host "Already pinned to 48 kHz (marker present) - nothing to do."
    return
}

# Back up the pristine upstream copy once (re-clone also restores it).
$bak = "$h.orig"
if (-not (Test-Path $bak)) { [System.IO.File]::WriteAllText($bak, $text) }

# --- New 48 kHz-only table bodies (literal here-strings; no interpolation) ----
$formatsNew = @'
static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE SpeakerHostPinSupportedDeviceFormats[] =
{
    //-------------------------------------------------
    // 0) 16-bit, Stereo, 48 kHz   (AudioPipe: 48 kHz only)
    //-------------------------------------------------
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                48000,
                48000 * 2 * 16 / 8,
                2 * 16 / 8,
                16,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            16,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    },
    //-------------------------------------------------
    // 1) 24-bit, Stereo, 48 kHz
    //-------------------------------------------------
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                48000,
                48000 * 2 * 24 / 8,
                2 * 24 / 8,
                24,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            24,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    },
    //-------------------------------------------------
    // 2) 32-bit, Stereo, 48 kHz
    //-------------------------------------------------
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                48000,
                48000 * 2 * 32 / 8,
                2 * 32 / 8,
                32,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            32,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    }
};
'@

$modesNew = @'
static
MODE_AND_DEFAULT_FORMAT SpeakerHostPinSupportedDeviceModes[] =
{
    // 0) 16-bit, Stereo, 48000 Hz
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerHostPinSupportedDeviceFormats[0].DataFormat
    },
    // 1) 24-bit, Stereo, 48000 Hz
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerHostPinSupportedDeviceFormats[1].DataFormat
    },
    // 2) 32-bit, Stereo, 48000 Hz
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerHostPinSupportedDeviceFormats[2].DataFormat
    }
};
'@

# Helper: replace exactly one Singleline match or throw.
function Replace-Once([string]$body, [string]$pattern, [string]$replacement, [string]$what) {
    $rx = [regex]::new($pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
    $count = $rx.Matches($body).Count
    if ($count -ne 1) {
        throw "expected exactly 1 match for $what but found $count - upstream table shape changed; update apply_48k.ps1."
    }
    # MatchEvaluator => replacement is treated literally ($ and & are not special).
    return $rx.Replace($body, { param($m) $replacement }, 1)
}

$text = Replace-Once $text `
    'static\s+KSDATAFORMAT_WAVEFORMATEXTENSIBLE\s+SpeakerHostPinSupportedDeviceFormats\[\]\s*=\s*\{.*?\n\};' `
    $formatsNew 'SpeakerHostPinSupportedDeviceFormats[]'

$text = Replace-Once $text `
    'static\s+MODE_AND_DEFAULT_FORMAT\s+SpeakerHostPinSupportedDeviceModes\[\]\s*=\s*\{.*?\n\};' `
    $modesNew 'SpeakerHostPinSupportedDeviceModes[]'

$text = Replace-Once $text `
    '[0-9]+,[ \t]*// MinimumSampleFrequency[^\r\n]*' `
    '48000,  // MinimumSampleFrequency (pinned to 48 kHz for AudioPipe)' 'MinimumSampleFrequency'

$text = Replace-Once $text `
    '[0-9]+[ \t]*// MaximumSampleFrequency[^\r\n]*' `
    '48000   // MaximumSampleFrequency (pinned to 48 kHz for AudioPipe)' 'MaximumSampleFrequency'

$text = Replace-Once $text `
    'SPEAKER_HOST_MAX_SAMPLE_RATE\s+192000' `
    'SPEAKER_HOST_MAX_SAMPLE_RATE        48000   // AudioPipe: pinned to 48 kHz' 'SPEAKER_HOST_MAX_SAMPLE_RATE'

# Stale doc comments that mention 192kHz (cosmetic; tolerate absence).
$text = [regex]::Replace($text, 'sample rates from 48kHz up to 192kHz',
                         'sample rate pinned to 48 kHz (AudioPipe)')
$text = [regex]::Replace($text, '48kHz through 192kHz', '48 kHz only (AudioPipe)')

# Safety net: no non-48k PCM rate literals should remain in the table.
$strays = [regex]::Matches($text, '\b(8000|11025|16000|22050|32000|44100|88200|96000|176400|192000|384000)\b')
if ($strays.Count -gt 0) {
    $vals = ($strays | ForEach-Object { $_.Value } | Sort-Object -Unique) -join ', '
    throw "non-48k sample-rate literals still present after patch: $vals"
}

[System.IO.File]::WriteAllText($h, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "speakerwavtable.h pinned to 48 kHz only (16/24/32-bit, stereo). Backup: $bak"
Write-Host "Now build:  .\build.ps1"
