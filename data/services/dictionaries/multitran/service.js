function languageId(value) {
    switch (String(value || "").toLowerCase()) {
        case "en": return 1;
        case "ru": return 2;
        case "de": return 3;
        case "fr": return 4;
        case "es": return 5;
        case "it": return 6;
        case "pt": return 7;
        case "nl": return 8;
        case "pl": return 9;
        case "uk": return 23;
        case "zh-cn": return 31;
        case "zh-tw": return 32;
        default: return 0;
    }
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: "Multitran: поддерживаются только словарные статьи" };
    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: "Пустое слово" };
    var from = languageId(params.from);
    var to = languageId(params.to);
    if (!from || !to)
        return { error: "Multitran: неподдерживаемая языковая пара" };
    var url = "https://www.multitran.com/m.exe?l1=" + from + "&l2=" + to
        + "&s=" + encodeURIComponent(word) + "&SHL=1";
    var response = http.get(url, {
        "Accept": "text/html,application/xhtml+xml",
        "Accept-Language": "ru,en;q=0.8"
    });
    if (response.error)
        return { error: "Multitran: " + response.error };
    if (response.status !== 200)
        return { error: "Multitran: HTTP " + response.status };
    return {
        html: "<div class=\"dict-entry\">" + response.body + "</div>",
        text: word,
        title: "Multitran — " + word,
        url: url
    };
}
