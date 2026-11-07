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
    var endpoint = String(value || "https://api.anthropic.com/v1/messages").trim();
    return endpoint.replace(/\/+$/, "");
}

function execute(capability, params) {
    if (!settings.api_key) {
        return { error: "Claude: укажите API Key в настройках." };
    }

    var prompts = loadJSON("prompts.json");
    var endpoint = normalizeEndpoint(settings.endpoint);
    var model = String(settings.model || "claude-sonnet-4-5").trim();
    var temperature = Number(settings.temperature);
    if (isNaN(temperature)) {
        temperature = 0.2;
    }

    var prompt = promptFor(capability, params, prompts);
    var body = {
        model: model,
        max_tokens: 4096,
        temperature: temperature,
        system: systemPromptFor(params),
        messages: [
            { role: "user", content: prompt }
        ]
    };

    var response = http.post(endpoint, body, {
        "x-api-key": String(settings.api_key),
        "anthropic-version": "2023-06-01",
        "Content-Type": "application/json"
    });
    if (response.error) {
        return { error: response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Claude: некорректный ответ сервера." };
    }

    if (response.status !== 200) {
        var message = data && data.error && data.error.message;
        return { error: message || ("Claude: HTTP " + response.status) };
    }

    var text = "";
    if (data && data.content) {
        for (var i = 0; i < data.content.length; i++) {
            if (data.content[i] && data.content[i].type === "text") {
                text += data.content[i].text || "";
            }
        }
    }
    if (!text) {
        return { error: "Claude: пустой ответ." };
    }

    if (capability === "dictionary") {
        return { html: "<p>" + escapeHtml(text) + "</p>", text: text };
    }
    return { text: text };
}
