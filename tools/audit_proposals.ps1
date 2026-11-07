Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;

public static class ColorMath {
    static double Lum(string hex) {
        hex = hex.TrimStart('#');
        double r = Convert.ToInt32(hex.Substring(0,2),16)/255.0;
        double g = Convert.ToInt32(hex.Substring(2,2),16)/255.0;
        double b = Convert.ToInt32(hex.Substring(4,2),16)/255.0;
        r = r <= 0.03928 ? r/12.92 : Math.Pow((r+0.055)/1.055, 2.4);
        g = g <= 0.03928 ? g/12.92 : Math.Pow((g+0.055)/1.055, 2.4);
        b = b <= 0.03928 ? b/12.92 : Math.Pow((b+0.055)/1.055, 2.4);
        return 0.2126*r + 0.7152*g + 0.0722*b;
    }
    public static double Contrast(string a, string b) {
        double l1 = Lum(a), l2 = Lum(b);
        if (l1 < l2) { double t = l1; l1 = l2; l2 = t; }
        return (l1+0.05)/(l2+0.05);
    }
    static int[] ToHsl(string hex) { return new int[0]; }
    // keeps hue/saturation, moves lightness until contrast target met on all backgrounds
    public static string Adjust(string hex, double target, string[] backgrounds) {
        hex = hex.TrimStart('#');
        double r = Convert.ToInt32(hex.Substring(0,2),16)/255.0;
        double g = Convert.ToInt32(hex.Substring(2,2),16)/255.0;
        double b = Convert.ToInt32(hex.Substring(4,2),16)/255.0;
        double max = Math.Max(r, Math.Max(g,b)), min = Math.Min(r, Math.Min(g,b));
        double l = (max+min)/2, s = 0, h = 0;
        double d = max-min;
        if (d > 0) {
            s = l > 0.5 ? d/(2-max-min) : d/(max+min);
            if (max == r) h = (g-b)/d + (g < b ? 6 : 0);
            else if (max == g) h = (b-r)/d + 2;
            else h = (r-g)/d + 4;
            h /= 6;
        }
        bool darken = Lum(hex) < Lum(backgrounds[0]);
        for (int step = 1; step <= 200; step++) {
            double nl = darken ? l - step*0.005 : l + step*0.005;
            if (nl < 0 || nl > 1) break;
            string cand = HslToHex(h, s, nl);
            bool ok = true;
            foreach (string bg in backgrounds) if (Contrast(cand, bg) < target) { ok = false; break; }
            if (ok) return "#" + cand;
        }
        return "#" + hex;
    }
    static string HslToHex(double h, double s, double l) {
        double q = l < 0.5 ? l*(1+s) : l+s-l*s, p = 2*l-q;
        double r = Hue(p,q,h+1.0/3), g = Hue(p,q,h), b = Hue(p,q,h-1.0/3);
        return string.Format("{0:x2}{1:x2}{2:x2}", (int)Math.Round(r*255), (int)Math.Round(g*255), (int)Math.Round(b*255));
    }
    static double Hue(double p, double q, double t) {
        if (t < 0) t += 1; if (t > 1) t -= 1;
        if (t < 1.0/6) return p + (q-p)*6*t;
        if (t < 0.5) return q;
        if (t < 2.0/3) return p + (q-p)*(2.0/3-t)*6;
        return p;
    }
}
'@

$themes = Get-Content resources\themes.json -Raw -Encoding utf8 | ConvertFrom-Json
$out = @()
foreach ($t in $themes) {
    $rows = @()
    foreach ($spec in @(
        @{role='muted';        old=$t.muted;        target=4.5; bgs=@($t.surface, $t.surfaceAlt)},
        @{role='accentText';   old=$t.accentText;   target=4.5; bgs=@($t.accentSoft, $t.surface)},
        @{role='success';      old=$t.success;      target=3.0; bgs=@($t.surfaceAlt, $t.surface)},
        @{role='danger';       old=$t.danger;       target=4.5; bgs=@($t.surface, $t.surfaceAlt)},
        @{role='borderStrong'; old=$t.borderStrong; target=3.0; bgs=@($t.surface, $t.surfaceAlt)}
    )) {
        $fails = $false
        foreach ($bg in $spec.bgs) { if ([ColorMath]::Contrast($spec.old, $bg) -lt $spec.target) { $fails = $true } }
        if (-not $fails) { continue }
        $new = [ColorMath]::Adjust($spec.old, $spec.target, $spec.bgs)
        $before = [math]::Round([math]::Min([ColorMath]::Contrast($spec.old, $spec.bgs[0]), [ColorMath]::Contrast($spec.old, $spec.bgs[1])), 2)
        $after = [math]::Round([math]::Min([ColorMath]::Contrast($new, $spec.bgs[0]), [ColorMath]::Contrast($new, $spec.bgs[1])), 2)
        $rows += ("{0}: {1} -> {2} ({3} -> {4})" -f $spec.role, $spec.old, $new, $before, $after)
    }

    $needFix = $false
    foreach ($bg in @($t.accent, $t.accentHover, $t.accentActive)) {
        if ([ColorMath]::Contrast($t.onAccent, $bg) -lt 4.5) { $needFix = $true }
    }
    if ($needFix) {
        $whiteOk = $true; $darkOk = $true
        foreach ($bg in @($t.accent, $t.accentHover, $t.accentActive)) {
            if ([ColorMath]::Contrast('#ffffff', $bg) -lt 4.5) { $whiteOk = $false }
            if ([ColorMath]::Contrast('#08131f', $bg) -lt 4.5) { $darkOk = $false }
        }
        if ($whiteOk) { $rows += ("onAccent: {0} -> #ffffff" -f $t.onAccent) }
        elseif ($darkOk) { $rows += ("onAccent: {0} -> #08131f" -f $t.onAccent) }
        else { $rows += ("onAccent: {0} -> ? (нет проходящего на всех трёх)" -f $t.onAccent) }
    }

    if ($rows.Count -gt 0) {
        $out += ("{0}: {1}" -f $t.name, ($rows -join '; '))
    }
}
$out | Set-Content -Encoding utf8 tools\audit_proposals.txt
$out
