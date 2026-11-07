Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;

public static class Audit2 {
    public static double Lum(string hex) {
        hex = hex.TrimStart('#');
        double r = Convert.ToInt32(hex.Substring(0,2),16)/255.0;
        double g = Convert.ToInt32(hex.Substring(2,2),16)/255.0;
        double b = Convert.ToInt32(hex.Substring(4,2),16)/255.0;
        r = r <= 0.03928 ? r/12.92 : Math.Pow((r+0.055)/1.055, 2.4);
        g = g <= 0.03928 ? g/12.92 : Math.Pow((g+0.055)/1.055, 2.4);
        b = b <= 0.03928 ? b/12.92 : Math.Pow((b+0.055)/1.055, 2.4);
        return 0.2126*r + 0.7152*g + 0.0722*b;
    }
    public static double Lightness(string hex) {
        double y = Lum(hex);
        return y > 0.008856 ? 116*Math.Pow(y, 1.0/3) - 16 : 903.3*y;
    }
    public static double Contrast(string a, string b) {
        double l1 = Lum(a), l2 = Lum(b);
        if (l1 < l2) { double t = l1; l1 = l2; l2 = t; }
        return (l1+0.05)/(l2+0.05);
    }
    public static double[] ToHsl(string hex) {
        hex = hex.TrimStart('#');
        double r = Convert.ToInt32(hex.Substring(0,2),16)/255.0;
        double g = Convert.ToInt32(hex.Substring(2,2),16)/255.0;
        double b = Convert.ToInt32(hex.Substring(4,2),16)/255.0;
        double max = Math.Max(r, Math.Max(g,b)), min = Math.Min(r, Math.Min(g,b));
        double l = (max+min)/2, s = 0, h = 0, d = max-min;
        if (d > 0) {
            s = l > 0.5 ? d/(2-max-min) : d/(max+min);
            if (max == r) h = (g-b)/d + (g < b ? 6 : 0);
            else if (max == g) h = (b-r)/d + 2;
            else h = (r-g)/d + 4;
            h /= 6;
        }
        return new double[]{h, s, l};
    }
    public static string FromHsl(double h, double s, double l) {
        if (s <= 0) { int v = (int)Math.Round(l*255); return string.Format("#{0:x2}{1:x2}{2:x2}", v, v, v); }
        double q = l < 0.5 ? l*(1+s) : l+s-l*s, p = 2*l-q;
        double r = Hue(p,q,h+1.0/3), g = Hue(p,q,h), b = Hue(p,q,h-1.0/3);
        return string.Format("#{0:x2}{1:x2}{2:x2}",
            (int)Math.Round(Math.Max(0,Math.Min(1,r))*255),
            (int)Math.Round(Math.Max(0,Math.Min(1,g))*255),
            (int)Math.Round(Math.Max(0,Math.Min(1,b))*255));
    }
    static double Hue(double p, double q, double t) {
        if (t < 0) t += 1; if (t > 1) t -= 1;
        if (t < 1.0/6) return p + (q-p)*6*t;
        if (t < 0.5) return q;
        if (t < 2.0/3) return p + (q-p)*(2.0/3-t)*6;
        return p;
    }
    static bool Passes(string c, double target, string[] bgs) {
        foreach (string bg in bgs) if (Contrast(c, bg) < target) return false;
        return true;
    }
    static double Margin(string c, double target, string[] bgs) {
        double m = 99;
        foreach (string bg in bgs) m = Math.Min(m, Contrast(c, bg) - target);
        return m;
    }
    // strict: fixed hue+saturation, minimal lightness move
    public static string Strict(string hex, double target, string[] bgs) {
        double[] hsl = ToHsl(hex);
        bool darken = Lum(hex) < Lum(bgs[0]);
        for (int step = 1; step <= 400; step++) {
            double nl = darken ? hsl[2] - step*0.0025 : hsl[2] + step*0.0025;
            if (nl < 0 || nl > 1) break;
            string cand = FromHsl(hsl[0], hsl[1], nl);
            if (Passes(cand, target, bgs)) return cand;
        }
        return hex;
    }
    // soft: allow small saturation drift to minimise the lightness change
    public static string Soft(string hex, double target, string[] bgs, out double ds) {
        double[] hsl = ToHsl(hex);
        double l0 = Lightness(hex);
        double best = 1e9; string bestHex = hex; double bestDs = 0;
        for (int si = -15; si <= 15; si++) {
            double s = Math.Max(0, Math.Min(1, hsl[1] + si*0.01));
            if (Math.Abs(s - hsl[1]) > 0.16) continue;
            bool darken = Lum(hex) < Lum(bgs[0]);
            for (int step = 1; step <= 400; step++) {
                double nl = darken ? hsl[2] - step*0.0025 : hsl[2] + step*0.0025;
                if (nl < 0 || nl > 1) break;
                string cand = FromHsl(hsl[0], s, nl);
                if (!Passes(cand, target, bgs)) continue;
                double dl = Math.Abs(Lightness(cand) - l0);
                if (dl < best - 0.01 || (Math.Abs(dl-best) <= 0.01 && Math.Abs(s-hsl[1]) < Math.Abs(bestDs))) {
                    best = dl; bestHex = cand; bestDs = s - hsl[1];
                }
                break;
            }
        }
        ds = bestDs;
        return bestHex;
    }
}
'@

$themes = Get-Content resources\themes.json -Raw -Encoding utf8 | ConvertFrom-Json
$rows = @()
foreach ($t in $themes) {
    $specs = @(
        @{n='9 muted';        k='muted';        tgt=4.5; bgs=@($t.surface, $t.surfaceAlt)},
        @{n='10 accentText';  k='accentText';   tgt=4.5; bgs=@($t.accentSoft, $t.surface)},
        @{n='12 danger';      k='danger';       tgt=4.5; bgs=@($t.surface, $t.surfaceAlt)},
        @{n='14 borderStrong';k='borderStrong'; tgt=3.0; bgs=@($t.surface, $t.surfaceAlt)}
    )
    foreach ($s in $specs) {
        $orig = $t.($s.k)
        $strict = [Audit2]::Strict($orig, $s.tgt, $s.bgs)
        if ($strict -eq $orig) { continue }
        $dL = [math]::Abs([Audit2]::Lightness($strict) - [Audit2]::Lightness($orig))
        if ($dL -le 0) { continue }
        $ds = 0.0
        $soft = [Audit2]::Soft($orig, $s.tgt, $s.bgs, [ref]$ds)
        $softDL = [math]::Abs([Audit2]::Lightness($soft) - [Audit2]::Lightness($orig))
        $margin = 99.0
        foreach ($bg in $s.bgs) { $margin = [math]::Min($margin, [Audit2]::Contrast($soft, $bg) - $s.tgt) }
        $rows += ("{0}|{1}|{2}|{3}|{4}|{5:F1}|{6:F1}|{7:F2}" -f `
            $t.name, $s.n, $orig, $strict, $soft, $dL, $softDL, $margin)
    }
}
$rows | Set-Content -Encoding utf8 tools\audit_soft.txt
$rows
