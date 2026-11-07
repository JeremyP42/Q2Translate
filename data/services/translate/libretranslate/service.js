function baseUrl() {
    var value = String(settings.endpoint || "http://localhost:5000").trim().replace(/\/+$/, "");
    if (/\/translate$/.test(value))
        value = value.substring(0, value.length - 10);
    return value;
}

function execute(capability, params) {
    if (capability !== "translate" && capability !== "detect")
        return { error: "LibreTranslate: неподдерживаемая операция " + capability };
    var text = String(params.text || "");
    if (!text)
        return { error: "Пустой текст" };

    var from = capability === "detect" ? "auto" : (params.from || "auto");
    var to = capability === "detect" ? "en" : (params.to || "ru");
    var body = { q: text, source: from, target: to, format: "text" };
    if (settings.api_key)
        body.api_key = String(settings.api_key);

    var response = http.post(baseUrl() + "/translate", body, {
        "Accept": "application/json",
        "Content-Type": "application/json"
    });
    if (response.error)
        return { error: "LibreTranslate: " + response.error };

    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "LibreTranslate: некорректный JSON" }; }

    if (response.status !== 200)
        return { error: "LibreTranslate: HTTP " + response.status + (data && data.error ? " — " + data.error : "") };
    if (!data || !data.translatedText)
        return { error: "LibreTranslate: пустой перевод" };
    return {
        text: data.translatedText,
        detected_lang: data.detectedLanguage && data.detectedLanguage.language
            ? data.detectedLanguage.language : (capability === "detect" ? from : undefined)
    };
}
