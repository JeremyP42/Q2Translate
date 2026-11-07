function escapeHtml(value) {
    return String(value)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/\n/g, "<br>");
}

function promptFor(capability, params, prompts) {
    var source = params.source_language_name || params.from_name || params.from || "auto";
    var target = params.to_name || params.to || "ru";
    var text = params.text || params.word || "";

    if (capability === "translate") {
        return (prompts.translate || "Translate from {source} to {target}:\n{text}")
            .replace("{source}", source)
            .replace("{target}", target)
            .replace("{text}", text) + (params.context ? "\n\nScene context (do not translate; keep names and terminology consistent):\n" + String(params.context).trim() : "") + (params.instruction ? "\n\n" + String(params.instruction).trim() : "");
    }
    if (capability === "summarize") {
        return (prompts.summarize || "Summarize:\n{text}").replace("{text}", text);
    }
    if (capability === "paraphrase") {
        return (prompts.paraphrase || "Rewrite:\n{text}").replace("{text}", text);
    }
    if (capability === "dictionary") {
        return (prompts.dictionary || "Explain \"{text}\" in {target}.")
            .replace("{text}", text)
            .replace("{target}", target);
    }
    return text;
}

function systemPromptFor(params) {
    var source = params.source_language_name || params.from_name || params.from || "auto";
    var target = params.to_name || params.to || "ru";
    var text = params.text || params.word || "";
    var prompt = String(settings.system_prompt || "").trim();
    if (!prompt) {
        prompt = "You are a precise assistant. Follow the user instruction exactly.";
    }
    return prompt
        .replace(/\{src\}/g, source)
        .replace(/\{dst\}/g, target)
        .replace(/\{text\}/g, text);
}

function normalizeEndpoint(value) {
    var endpoint = String(value || "https://generativelanguage.googleapis.com/v1beta/models").trim();
    return endpoint.replace(/\/+$/, "");
}

function requestUrl() {
    var base = normalizeEndpoint(settings.endpoint);
    if (/:generateContent$/i.test(base)) {
        return base;
    }
    var model = String(settings.model || "gemini-2.5-flash").trim();
    return base + "/" + encodeURIComponent(model) + ":generateContent";
}

function execute(capability, params) {
    if (!settings.api_key) {
        return { error: "Gemini: укажите API Key в настройках." };
    }

    var prompts = loadJSON("prompts.json");
    var endpoint = requestUrl();
    var temperature = Number(settings.temperature);
    if (isNaN(temperature)) {
        temperature = 0.2;
    }

    var prompt = promptFor(capability, params, prompts);
    var body = {
        system_instruction: {
            parts: [{ text: systemPromptFor(params) }]
        },
        contents: [
            { role: "user", parts: [{ text: prompt }] }
        ],
        generationConfig: {
            temperature: temperature
        }
    };

    var response = http.post(endpoint, body, {
        "x-goog-api-key": String(settings.api_key),
        "Content-Type": "application/json"
    });
    if (response.error) {
        return { error: response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Gemini: некорректный ответ сервера." };
    }

    if (response.status !== 200) {
        var message = data && data.error && data.error.message;
        return { error: message || ("Gemini: HTTP " + response.status) };
    }

    var text = "";
    if (data && data.candidates) {
        for (var i = 0; i < data.candidates.length; i++) {
            var candidate = data.candidates[i];
            var parts = candidate && candidate.content && candidate.content.parts;
            if (!parts) {
                continue;
            }
            for (var j = 0; j < parts.length; j++) {
                if (parts[j] && parts[j].text) {
                    text += parts[j].text;
                }
            }
            if (text) {
                break;
            }
        }
    }
    if (!text) {
        return { error: "Gemini: пустой ответ." };
    }

    if (capability === "dictionary") {
        return { html: "<p>" + escapeHtml(text) + "</p>", text: text };
    }
    return { text: text };
}
