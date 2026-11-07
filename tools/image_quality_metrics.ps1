# Summarises image-layout dumps produced by
#   Q2Translate.exe --translate-image <file> <target> <source> --image-layout-dump <file>
#
# Usage: pwsh -File tools/image_quality_metrics.ps1 -Directory work/test/_runs/src

param(
    [Parameter(Mandatory = $true)]
    [string]$Directory
)

$dumps = Get-ChildItem -Path $Directory -Filter '*.json' -File |
    Where-Object { $_.Name -notlike '*.masks.png' } | Sort-Object Name
if (-not $dumps) {
    Write-Error "No layout dump (*.json) files found in $Directory"
    exit 1
}

$rows = @()
foreach ($dump in $dumps) {
    $data = Get-Content $dump.FullName -Raw | ConvertFrom-Json
    $blocks = @($data.blocks)
    if ($blocks.Count -eq 0) {
        continue
    }

    $fonts = @($blocks | ForEach-Object { [int]$_.fontPx } | Sort-Object)
    $masked = @($blocks | Where-Object { $_.masked }).Count
    $overflow = @($blocks | Where-Object { $_.overflow }).Count
    $tiny = @($blocks | Where-Object { [int]$_.fontPx -lt 10 }).Count
    $skipped = @($blocks | Where-Object { [int]$_.fontPx -eq 0 }).Count
    $median = $fonts[[int]([math]::Floor($fonts.Count / 2))]

    $rows += [pscustomobject]@{
        Page     = $dump.BaseName -replace '\.dump\.json$', ''
        Blocks   = $blocks.Count
        Masked   = $masked
        MinFont  = ($fonts | Measure-Object -Minimum).Minimum
        Median   = $median
        Tiny     = $tiny
        Overflow = $overflow
        Skipped  = $skipped
    }
}

$rows | Format-Table -AutoSize

$totalBlocks = ($rows | Measure-Object -Property Blocks -Sum).Sum
$totalTiny = ($rows | Measure-Object -Property Tiny -Sum).Sum
$totalOverflow = ($rows | Measure-Object -Property Overflow -Sum).Sum
$totalSkipped = ($rows | Measure-Object -Property Skipped -Sum).Sum
$maskedBlocks = ($rows | Measure-Object -Property Masked -Sum).Sum

Write-Host ""
Write-Host ("pages={0} blocks={1} masked={2} ({3:P0}) tiny(<10px)={4} ({5:P1}) overflow={6} skipped={7}" -f `
        $rows.Count, $totalBlocks, $maskedBlocks, ($maskedBlocks / [math]::Max(1, $totalBlocks)), `
        $totalTiny, ($totalTiny / [math]::Max(1, $totalBlocks)), $totalOverflow, $totalSkipped)
