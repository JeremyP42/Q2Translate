function providerCode(value) {
    var code = String(value || "").toLowerCase();
    if (code === "auto" || !code) return "";
    if (code === "zh-cn" || code === "zh-tw") return "zh";
    return code.split("-")[0];
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "Reverso: поддерживается только перевод" };
    var text = String(params.text || "");
    if (!text)
        return { error: "Пустой текст" };
    var from = providerCode(params.from);
    var to = providerCode(params.to);
    if (!from || !to)
        return { error: "Reverso не поддерживает автоопределение языка" };

    var body = {
        format: "text",
        from: from,
        input: text,
        options: {
            contextResults: false,
            languageDetection: true,
            origin: "reversomobile",
            sentenceSplitter: false
        },
        to: to
    };
    var response = http.post("https://api.reverso.net/translate/v1/translation", JSON.stringify(body), {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "Origin": "https://www.reverso.net",
        "Referer": "https://www.reverso.net/",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36"
    });
    if (response.error) {
        if (response.status === 403)
            return { error: "Reverso: HTTP 403 (Cloudflare)" };
        return { error: "Reverso: " + response.error };
    }
    var data;
    try { data = json.parse(response.body); }
    catch (error) {
        if (response.status === 403)
            return { error: "Reverso: HTTP 403 (Cloudflare)" };
        return { error: "Reverso: не удалось разобрать JSON" };
    }
    var translation = Array.isArray(data.translation)
        ? data.translation[0] : data.translation;
    if (response.status !== 200 || !translation || data.error || data.errors) {
        var message = data.error || data.message || data.title;
        if (!message && data.errors) {
            for (var key in data.errors) {
                if (data.errors[key] && data.errors[key].length) {
                    message = data.errors[key][0];
                    break;
                }
            }
        }
        return { error: "Reverso: " + (message || ("HTTP " + response.status)) };
    }
    var detected = data.languageDetection && data.languageDetection.detectedLanguage;
    return { text: translation, detected_lang: detected };
}
