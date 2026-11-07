/*
 * Multi connector.
 *
 * Runs several independent web translators and returns the result with the
 * best simple quality score.  The score checks length, preserved numbers and
 * links, sentence punctuation and repeated-character noise.  Engines can be
 * limited with the comma-separated ActiveServices setting.
 */

function multiCode(value, fallback) {
    var code = String(value == null ? "" : value).trim().toLowerCase();
    if (!code || code === "auto")
        return fallback || "auto";
    if (code === "zh-cn" || code === "zh-tw")
        return "zh";
    return code.split("-")[0];
}

function multiRequestError(label, response) {
    return label + ": " + (response.error || ("HTTP " + response.status));
}

function googleEngine(text, from, to) {
    var url = "https://translate.googleapis.com/translate_a/single?client=gtx&dt=t"
        + "&sl=" + encodeURIComponent(from)
        + "&tl=" + encodeURIComponent(to)
        + "&q=" + encodeURIComponent(text);
    var response = http.get(url, { "Accept": "application/json,text/plain,*/*" });
    if (response.error || response.status !== 200)
        return { error: multiRequestError("Google", response) };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "Google: некорректный ответ" }; }
    if (!data || !data[0])
        return { error: "Google: пустой ответ" };
    var out = "";
    for (var i = 0; i < data[0].length; ++i) {
        if (data[0][i] && data[0][i][0])
            out += data[0][i][0];
    }
    if (!out)
        return { error: "Google: перевод не получен" };
    return { text: out, detected_lang: data[2] || from };
}

function yandexEngine(text, from, to) {
    var uuid = "";
    for (var u = 0; u < 32; ++u)
        uuid += (Math.random() * 16 | 0).toString(16);
    var url = "https://translate.yandex.net/api/v1/tr.json/translate?uuid=" + encodeURIComponent(uuid)
        + "&srv=android&lang=" + encodeURIComponent(from + "-" + to)
        + "&reason=auto&format=text&yu=2210680511641235828";
    var response = http.post(url, "text=" + encodeURIComponent(text), {
        "Accept": "application/json",
        "Content-Type": "application/x-www-form-urlencoded",
        "Referer": "https://translate.yandex.com/",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
    });
    if (response.error || response.status !== 200)
        return { error: multiRequestError("Yandex", response) };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "Yandex: некорректный ответ" }; }
    if (!data || Number(data.code) !== 200 || !data.text)
        return { error: "Yandex: перевод не получен" };
    var out = data.text && data.text.join ? data.text.join("\n") : String(data.text);
    var pair = String(data.lang || "").split("-");
    return { text: out, detected_lang: pair[0] || from };
}

function laraEngine(text, from, to) {
    var body = JSON.stringify({
        q: text,
        source: from === "auto" ? "" : from,
        target: to,
        instructions: []
    });
    var response = http.post("https://webapi.laratranslate.com/translate", body, {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "Origin": "https://app.laratranslate.com",
        "Referer": "https://app.laratranslate.com/"
    });
    if (response.error || response.status !== 200)
        return { error: multiRequestError("Lara", response) };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "Lara: некорректный ответ" }; }
    var content = data && data.content ? data.content : data;
    var out = content && content.translation;
    if (!out)
        return { error: "Lara: перевод не получен" };
    return { text: out, detected_lang: content.source_language || from };
}

function translateEuEngine(text, from, to) {
    var body = "txt=" + encodeURIComponent(text)
        + "&from=" + encodeURIComponent(from === "auto" ? "" : from)
        + "&to=" + encodeURIComponent(to);
    var response = http.post("https://www.translate.eu/libre/", body, {
        "Accept": "message/x-jl-formresult,application/json",
        "Content-Type": "application/x-www-form-urlencoded",
        "Origin": "https://www.translate.eu",
        "Referer": "https://www.translate.eu/"
    });
    if (response.error || response.status !== 200)
        return { error: multiRequestError("TranslateEU", response) };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "TranslateEU: некорректный ответ" }; }
    if (!data || !data.tr)
        return { error: "TranslateEU: перевод не получен" };
    return {
        text: String(data.tr).replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim(),
        detected_lang: from
    };
}

function myMemoryEngine(text, from, to, detected) {
    var source = detected && detected !== "auto" ? detected : "";
    if (!source)
        return { error: "MyMemory: нужен исходный язык" };
    var url = "https://api.mymemory.translated.net/get?q=" + encodeURIComponent(text)
        + "&langpair=" + encodeURIComponent(source + "|" + to);
    var response = http.get(url, { "Accept": "application/json" });
    if (response.error || response.status !== 200)
        return { error: multiRequestError("MyMemory", response) };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "MyMemory: некорректный ответ" }; }
    if (!data || !data.responseData || !data.responseData.translatedText)
        return { error: "MyMemory: перевод не получен" };
    return { text: data.responseData.translatedText, detected_lang: source };
}

function qualityScore(source, result) {
    if (!result || !result.text)
        return -1;
    var text = String(result.text);
    if (/^\s*\[E\]/i.test(text) || /\b(timeout|unavailable|no data|error)\b/i.test(text))
        return -1;
    var score = 20;
    var ratio = text.length / Math.max(1, source.length);
    if (ratio >= 0.35 && ratio <= 3.0)
        score += 25;
    else if (ratio >= 0.2 && ratio <= 5.0)
        score += 10;

    var numbers = source.match(/\d+/g) || [];
    if (numbers.length) {
        var kept = 0;
        for (var i = 0; i < numbers.length; ++i) {
            if (text.indexOf(numbers[i]) >= 0)
                kept++;
        }
        score += Math.round(20 * kept / numbers.length);
    } else {
        score += 10;
    }

    var links = source.match(/https?:\/\/\S+/g) || [];
    if (links.length) {
        var keptLinks = 0;
        for (var j = 0; j < links.length; ++j) {
            if (text.indexOf(links[j]) >= 0)
                keptLinks++;
        }
        score += Math.round(15 * keptLinks / links.length);
    } else {
        score += 10;
    }

    if (!/(.)\1{4,}/.test(text))
        score += 10;
    if (/\S/.test(text))
        score += 5;
    return score;
}

function activeEngineIds() {
    var raw = String(settings.ActiveServices || "").trim();
    if (!raw)
        return ["google", "yandex", "lara", "translateeu", "mymemory"];
    var parts = raw.split(/[\s,;]+/);
    var ids = [];
    for (var i = 0; i < parts.length; ++i) {
        var id = parts[i].toLowerCase();
        if (id && ids.indexOf(id) < 0)
            ids.push(id);
    }
    return ids;
}

function detectLanguage(text) {
    var result = googleEngine(text, "auto", "en");
    if (!result.error)
        return { detected_lang: result.detected_lang || "auto" };
    var yandex = yandexEngine(text, "auto", "en");
    if (!yandex.error)
        return { detected_lang: yandex.detected_lang || "auto" };
    return { error: "Multi: язык не определён" };
}

function execute(capability, params) {
    var text = String(params.text || params.word || "").trim();
    if (!text)
        return { error: "Multi: пустой текст" };
    if (capability === "detect")
        return detectLanguage(text);
    if (capability !== "translate")
        return { error: "Multi: неподдерживаемая возможность " + capability };

    var from = multiCode(params.from, "auto");
    var to = multiCode(params.to, "ru");
    var detected = multiCode(params.detected_from, "");
    var ids = activeEngineIds();
    var candidates = [];
    var errors = [];

    for (var i = 0; i < ids.length; ++i) {
        var id = ids[i];
        var result = null;
        if (id === "google")
            result = googleEngine(text, from, to);
        else if (id === "yandex")
            result = yandexEngine(text, from, to);
        else if (id === "lara")
            result = laraEngine(text, from, to);
        else if (id === "translateeu")
            result = translateEuEngine(text, from, to);
        else if (id === "mymemory")
            result = myMemoryEngine(text, from, to, detected);
        else
            continue;

        if (result && result.error) {
            errors.push(result.error);
            continue;
        }
        var score = qualityScore(text, result);
        if (score >= 0) {
            candidates.push({ result: result, score: score, engine: id });
            // A confident result is enough; querying every remaining engine
            // only adds latency.  Low-confidence results still fall through
            // to the backup engines below.
            if (score >= 80)
                break;
        }
    }

    if (!candidates.length)
        return { error: "Multi: " + (errors.length ? errors.join("; ") : "ни один движок не ответил") };

    candidates.sort(function (a, b) { return b.score - a.score; });
    var best = candidates[0].result;
    if (!detected || detected === "auto") {
        for (var c = 0; c < candidates.length; ++c) {
            var code = candidates[c].result.detected_lang;
            if (code && code !== "auto") {
                detected = code;
                break;
            }
        }
    }
    return {
        text: best.text,
        detected_lang: detected || best.detected_lang || from
    };
}
