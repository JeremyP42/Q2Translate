$themes = Get-Content resources\themes.json -Raw -Encoding utf8 | ConvertFrom-Json

function Get-Lum([string]$hex) {
    $hex = $hex.TrimStart('#')
    $r = [Convert]::ToInt32($hex.Substring(0,2),16) / 255.0
    $g = [Convert]::ToInt32($hex.Substring(2,2),16) / 255.0
    $b = [Convert]::ToInt32($hex.Substring(4,2),16) / 255.0
    $r = if ($r -le 0.03928) { $r / 12.92 } else { [math]::Pow(($r + 0.055) / 1.055, 2.4) }
    $g = if ($g -le 0.03928) { $g / 12.92 } else { [math]::Pow(($g + 0.055) / 1.055, 2.4) }
    $b = if ($b -le 0.03928) { $b / 12.92 } else { [math]::Pow(($b + 0.055) / 1.055, 2.4) }
    return (0.2126 * $r + 0.7152 * $g + 0.0722 * $b)
}

function Get-Contrast([string]$a, [string]$b) {
    $l1 = Get-Lum $a
    $l2 = Get-Lum $b
    if ($l1 -lt $l2) { $t = $l1; $l1 = $l2; $l2 = $t }
    return [math]::Round((($l1 + 0.05) / ($l2 + 0.05)), 2)
}

function Blend([string]$fgHex, [double]$alpha, [string]$bgHex) {
    $f = $fgHex.TrimStart('#'); $bg = $bgHex.TrimStart('#')
    $out = ''
    for ($i = 0; $i -lt 6; $i += 2) {
        $fc = [Convert]::ToInt32($f.Substring($i,2),16)
        $bc = [Convert]::ToInt32($bg.Substring($i,2),16)
        $v = [int][math]::Round($fc * $alpha + $bc * (1 - $alpha))
        $out += '{0:x2}' -f $v
    }
    return '#' + $out
}

$roles = @(
    @{n='text/surface';                 f='text';          b='surface'},
    @{n='text/surfaceAlt';              f='text';          b='surfaceAlt'},
    @{n='text/surfaceHover';            f='text';          b='surfaceHover'},
    @{n='textSecondary/surface';        f='textSecondary'; b='surface'},
    @{n='textSecondary/surfaceAlt';     f='textSecondary'; b='surfaceAlt'},
    @{n='muted/surface (hints)';        f='muted';         b='surface'},
    @{n='muted/surfaceAlt (disabled)';  f='muted';         b='surfaceAlt'},
    @{n='muted/surfaceHover (tr.disabled)'; f='muted';     b='surfaceHover'},
    @{n='accentText/surface (tabs)';    f='accentText';    b='surface'},
    @{n='accentText/accentSoft (hover+sel)'; f='accentText'; b='accentSoft'},
    @{n='text/accentSoft';              f='text';          b='accentSoft'},
    @{n='onAccent/accent (primary)';    f='onAccent';      b='accent'},
    @{n='onAccent/accentHover';         f='onAccent';      b='accentHover'},
    @{n='onAccent/accentActive';        f='onAccent';      b='accentActive'},
    @{n='danger/surface';               f='danger';        b='surface'},
    @{n='success/surfaceAlt (dot)';     f='success';       b='surfaceAlt'},
    @{n='tooltipText/tooltipBg';        f='tooltipText';   b='tooltipBg'}
)

$report = @()
foreach ($t in $themes) {
    $report += "=== $($t.name) ==="
    foreach ($r in $roles) {
        $fg = $t.($r.f); $bg = $t.($r.b)
        $cr = Get-Contrast $fg $bg
        $mark = ''
        if ($cr -lt 3) { $mark = '  << FAIL(3:1)' } elseif ($cr -lt 4.5) { $mark = '  << below 4.5' }
        $report += ("{0,-40} {1} / {2} = {3}{4}" -f $r.n, $fg, $bg, $cr, $mark)
    }
    # secondary text emulation used by popup (0.72 alpha of text over surface)
    $faded = Blend $t.text 0.72 $t.surface
    $cr = Get-Contrast $faded $t.surface
    $mark = if ($cr -lt 4.5) { '  << below 4.5' } else { '' }
    $report += ("{0,-40} {1} / {2} = {3}{4}" -f 'popup fadedText 0.72/surface', $faded, $t.surface, $cr, $mark)
    # borders as non-text UI (3:1)
    foreach ($pair in @(@('borderStrong/surface','borderStrong','surface'), @('border/surface','border','surface'), @('scrollbar/surface','scrollbar','surface'))) {
        $cr = Get-Contrast $t.($pair[1]) $t.($pair[2])
        $mark = if ($cr -lt 3) { '  << below 3' } else { '' }
        $report += ("{0,-40} {1} / {2} = {3}{4}" -f $pair[0], $t.($pair[1]), $t.($pair[2]), $cr, $mark)
    }
    $report += ''
}
$report | Set-Content -Encoding utf8 contrast_report.txt
$report | Select-String 'FAIL|below' | ForEach-Object { $_.Line }
"---- total lines: $($report.Count)"
