function execute(capability, params) {
    if (capability !== "translate") {
        return { error: "DeepL: неподдерживаемая возможность " + capability };
    }
    if (!settings.api_key) {
        return { error: "DeepL: укажите API Key в настройках сервиса" };
    }

    var text = params.text || "";
    if (!text) {
        return { error: "Пустой текст" };
    }

    var endpoint = settings.endpoint || "https://api-free.deepl.com/v2/translate";
    var from = params.from || "";
    var body = "text=" + encodeURIComponent(text) +
        "&target_lang=" + encodeURIComponent((params.to || "ru").toUpperCase());
    if (from && from !== "auto") {
        body += "&source_lang=" + encodeURIComponent(from.split("-")[0].toUpperCase());
    }
    if (settings.formality && settings.formality !== "default") {
        body += "&formality=" + encodeURIComponent(settings.formality);
    }

    var response = http.post(endpoint, body, {
        "Authorization": "DeepL-Auth-Key " + settings.api_key,
        "Content-Type": "application/x-www-form-urlencoded"
    });
    if (response.error) {
        return { error: response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "DeepL: некорректный ответ сервера" };
    }
    if (response.status !== 200 || !data.translations || !data.translations.length) {
        return { error: (data && data.message) || ("DeepL: HTTP " + response.status) };
    }

    return {
        text: data.translations[0].text,
        detected_lang: data.translations[0].detected_source_language || from
    };
}
