function escapeHtml(value) {
    return String(value || "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
}

function execute(capability, params) {
    if (capability !== "dictionary")
        return { error: "Urban Dictionary: поддерживаются только словарные статьи" };
    var word = String(params.word || params.text || "").trim();
    if (!word)
        return { error: "Пустое слово" };
    var url = "https://api.urbandictionary.com/v0/define?term=" + encodeURIComponent(word);
    var response = http.get(url, { "Accept": "application/json" });
    if (response.error)
        return { error: "Urban Dictionary: " + response.error };
    var data;
    try { data = json.parse(response.body); }
    catch (error) { return { error: "Urban Dictionary: некорректный JSON" }; }
    if (response.status !== 200 || !data || !data.list || !data.list.length)
        return { error: "Urban Dictionary: слово не найдено" };
    var html = "<div class=\"dict-entry\"><div class=\"dict-pos\">" + escapeHtml(word) + "</div><ol class=\"dict-defs\">";
    for (var i = 0; i < data.list.length; ++i) {
        var item = data.list[i];
        html += "<li>" + escapeHtml(item.definition || "").replace(/\r?\n/g, "<br>");
        if (item.example)
            html += "<div class=\"dict-example\">" + escapeHtml(item.example).replace(/\r?\n/g, "<br>") + "</div>";
        html += "</li>";
    }
    html += "</ol></div>";
    return { html: html, text: word, title: "Urban Dictionary — " + word, url: "https://www.urbandictionary.com/define.php?term=" + encodeURIComponent(word) };
}
