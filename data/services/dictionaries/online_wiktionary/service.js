function execute(capability, params) {
    if (capability !== "dictionary") {
        return { error: "Wiktionary: неподдерживаемая возможность " + capability };
    }

    var word = (params.word || params.text || "").trim();
    if (!word) {
        return { error: "Пустое слово" };
    }

    var language = settings.language || "en";
    if (params.from && params.from !== "auto") {
        language = params.from.split("-")[0];
    }

    var url = "https://" + language + ".wiktionary.org/api/rest_v1/page/definition/" +
        encodeURIComponent(word.replace(/\s+/g, "_"));

    var response = http.get(url, { "Accept": "application/json" });
    if (response.status === 404) {
        return { error: "Wiktionary: слово не найдено" };
    }
    if (response.error) {
        return { error: response.error };
    }
    if (response.status !== 200) {
        return { error: "Wiktionary: HTTP " + response.status };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Wiktionary: некорректный ответ сервера" };
    }

    var html = "";
    for (var code in data) {
        var sections = data[code];
        if (!sections || !sections.length) {
            continue;
        }
        for (var sectionIndex = 0; sectionIndex < sections.length; ++sectionIndex) {
            var section = sections[sectionIndex];
            html += "<div class=\"dict-entry\">";
            html += "<div class=\"dict-pos\">" + (section.partOfSpeech || code) + "</div>";
            var definitions = section.definitions || [];
            html += "<ol class=\"dict-defs\">";
            for (var index = 0; index < definitions.length; ++index) {
                html += "<li>" + (definitions[index].definition || "") + "</li>";
            }
            html += "</ol></div>";
        }
    }

    if (!html) {
        return { error: "Wiktionary: определение не найдено" };
    }

    return {
        html: html,
        text: word,
        title: "Wiktionary — " + word,
        url: "https://" + language + ".wiktionary.org/wiki/" + encodeURIComponent(word.replace(/\s+/g, "_"))
    };
}
